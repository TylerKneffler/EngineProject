#include "Game/Application/GameApplication.h"

#include "Game/Startup/GameProjectStartup.h"
#include "Game/Startup/RendererStartup.h"
#include "Game/Benchmark/GameBenchmark.h"
#include "Core/ProjectLoader.h"
#include "Core/Window.h"
#include "Core/Startup/StartupSplash.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/SceneManager.h"
#include "Core/Timing/FramePacer.h"
#include "Core/Input/NativeInputSnapshot.h"
#include "Core/Rendering/RenderFramePlan.h"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/Utilities/Rotate.h"
#include "Core/Assets/Scripts/Controllers/FirstPersonController.h"
#include "Core/Assets/Scripts/Gameplay/MainMenuGameManager.h"
#include "Core/Assets/Scripts/Portals/PortalSplitAfterDelay.h"
#include "Core/Assets/Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Serialization/SceneSerializer.h"
#endif

namespace Engine::Game
{
namespace
{
std::string CsvField(const std::string& value)
{
    std::string escaped = "\"";
    for (char character : value)
    {
        if (character == '"') escaped += '"';
        escaped += character;
    }
    escaped += '"';
    return escaped;
}

void RegisterGameComponents()
{
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
    ::Engine::Serialization::RegisterComponentType<Rotate>("Rotate");
    ::Engine::Serialization::RegisterComponentType<FirstPersonController>("FirstPersonController");
    ::Engine::Serialization::RegisterComponentType<MainMenuGameManager>("MainMenuGameManager");
    ::Engine::Serialization::RegisterComponentType<PortalSplitAfterDelay>(
        "PortalSplitAfterDelay");
    ::Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
#endif
}

std::unique_ptr<::Engine::Renderers::IGameRenderer> CreateRenderer(
    const Engine::Model::ProjectSettings& settings,
    ::Engine::Core::Window& window, bool interactive)
{
    try
    {
        std::unique_ptr<::Engine::Renderers::IGameRenderer> renderer =
            ::Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
        if (!renderer)
            throw std::runtime_error("The renderer factory returned no renderer.");
        if (!renderer->Init(window.GetHWND(), window.GetWidth(), window.GetHeight()))
            throw std::runtime_error("Renderer initialization returned false.");
        return renderer;
    }
    catch (const std::exception& error)
    {
        if (!interactive) throw;
        const std::string message = "Failed to initialize the " +
            settings.gameRenderingAPI + " renderer.\n\n"
            "Please ensure it is installed, supported by your GPU, and that "
            "your graphics driver is current.\n\nDetails: " + error.what();
        MessageBoxA(window.GetHWND(), message.c_str(),
            "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }
}

// The scene owner records draw commands after the update. The graphics owner
// replays those value arguments without reading objects or components.
class SimulationLoop
{
public:
    struct TickResult
    {
        struct Timings
        {
            double simulationUpdateMs = 0.0;
            double updateHandoffWaitMs = 0.0;
            double scenePreparationMs = 0.0;
            double commandRecordingMs = 0.0;
            double prepareHandoffWaitMs = 0.0;
        } timings;
        std::chrono::steady_clock::time_point startedAt;
        ::Engine::Core::NativeInputSnapshot input;
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> computeCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
        GameBenchmarkSceneSample benchmarkScene;
        double benchmarkCollectionMs = 0, intervalMs = 0, residencyWaitMs = 0;
        uint64_t benchmarkFrame = UINT64_MAX;
    };
    explicit SimulationLoop(::Engine::Scene::Scene& scene, bool collectBenchmark)
        : m_scene(scene), m_windowThread(std::this_thread::get_id()),
          m_collectBenchmark(collectBenchmark)
    {
        m_worker = std::thread([this] { Run(); });
        m_scene.TransferOwnerTo(m_worker.get_id());
        {
            std::lock_guard lock(m_mutex);
            m_ready = true;
        }
        m_wake.notify_one();
    }

    ~SimulationLoop() { Stop(); }
    SimulationLoop(const SimulationLoop&) = delete;
    SimulationLoop& operator=(const SimulationLoop&) = delete;

    void BeginUpdate(float deltaTime,
        ::Engine::Core::NativeInputSnapshot input)
    {
        std::unique_lock lock(m_mutex);
        if (m_stopping || m_busy)
            throw std::logic_error("Simulation handoff already has a frame");
        m_deltaTime = deltaTime;
        m_frameStartedAt = std::chrono::steady_clock::now();
        m_timings = {};
        m_input = std::move(input);
        m_busy = true;
        m_work = Work::Update;
        m_wake.notify_one();
    }

    TickResult FinishFrame(bool renderFrame, uint32_t previousOcclusionCulled)
    {
        std::unique_lock lock(m_mutex);
        if (!m_busy)
            throw std::logic_error("Simulation update was not started");
        auto waitStart = std::chrono::steady_clock::now();
        m_wake.wait(lock, [this] { return m_updateCompleted; });
        m_timings.updateHandoffWaitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        m_updateCompleted = false;
        if (m_failure)
        {
            m_busy = false;
            auto failure = std::exchange(m_failure, nullptr);
            std::rethrow_exception(failure);
        }
        if (!renderFrame)
        {
            m_busy = false;
            TickResult result;
            result.timings = m_timings;
            result.startedAt = m_frameStartedAt;
            result.input = std::move(m_input);
            return result;
        }
        m_previousOcclusionCulled = previousOcclusionCulled;
        m_work = Work::Prepare;
        m_wake.notify_one();
        waitStart = std::chrono::steady_clock::now();
        m_wake.wait(lock, [this] { return m_completed; });
        m_timings.prepareHandoffWaitMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        m_completed = false;
        m_busy = false;
        if (m_failure)
        {
            auto failure = std::exchange(m_failure, nullptr);
            std::rethrow_exception(failure);
        }
        TickResult result;
        result.timings = m_timings;
        result.startedAt = m_frameStartedAt;
        result.input = std::move(m_input);
        result.computeCommands = std::move(m_computeCommands);
        result.snapshot = std::move(m_snapshot);
        result.retired = std::move(m_retired);
        result.shadowCommands = std::move(m_shadowCommands);
        result.sceneCommands = std::move(m_sceneCommands);
        result.benchmarkScene = std::move(m_benchmarkScene);
        result.benchmarkCollectionMs = m_benchmarkCollectionMs;
        return result;
    }

    void Stop()
    {
        if (!m_worker.joinable()) return;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_worker.join();
    }

private:
    enum class Work { None, Update, Prepare };
    void Run()
    {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [this] { return m_ready; });
        while (true)
        {
            m_wake.wait(lock, [this]
            { return m_work != Work::None || m_stopping; });
            if (m_stopping) break;
            const Work work = std::exchange(m_work, Work::None);
            lock.unlock();
            std::exception_ptr failure;
            try
            {
                ::Engine::Core::SetCurrentNativeInputSnapshot(&m_input);
                if (work == Work::Update)
                {
                    const auto updateStart = std::chrono::steady_clock::now();
                    m_scene.Update(m_deltaTime);
                    if (::Engine::Core::SceneManager::ProcessPendingSceneLoad() &&
                        m_input.clientWidth && m_input.clientHeight)
                        m_scene.PrepareNativeUiLayout(
                            static_cast<float>(m_input.clientWidth) /
                            static_cast<float>(m_input.clientHeight));
                    m_timings.simulationUpdateMs = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() - updateStart).count();
                }
                else
                {
                    const auto prepareStart = std::chrono::steady_clock::now();
                    m_scene.PrepareRenderFrame();
                    m_timings.scenePreparationMs = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
                    const auto recordStart = std::chrono::steady_clock::now();
                    auto drawPacket = m_scene.TakeNativeRenderPacket();
                    auto computeCommands = std::make_shared<
                        ::Engine::Graphics::GraphicsCommandPacket>();
                    auto shadowCommands = std::make_shared<
                        ::Engine::Graphics::GraphicsCommandPacket>();
                    auto sceneCommands = std::make_shared<
                        ::Engine::Graphics::GraphicsCommandPacket>();
                    m_scene.RecordGpuParticleSimulation(computeCommands.get());
                    if (m_input.clientWidth && m_input.clientHeight)
                        if (auto* gameCamera = m_scene.FindGameCamera())
                        {
                            const float aspect = static_cast<float>(m_input.clientWidth) /
                                static_cast<float>(m_input.clientHeight);
                            m_scene.Render(shadowCommands.get(), aspect,
                                gameCamera, false, m_input.clientWidth,
                                m_input.clientHeight, true, drawPacket.get());
                            m_scene.Render(sceneCommands.get(), aspect,
                                gameCamera, false, m_input.clientWidth,
                                m_input.clientHeight, false,
                                drawPacket.get());
                        }
                    m_scene.RetainNativePacketMeshBuffers(*drawPacket,
                        *shadowCommands);
                    m_scene.SetLastNativeOcclusionCulledCount(
                        m_previousOcclusionCulled);
                    m_timings.commandRecordingMs = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() - recordStart).count();
                    if (m_collectBenchmark)
                    {
                        const auto collectionStart = std::chrono::steady_clock::now();
                        m_benchmarkScene = GameBenchmark::CollectScene(m_scene);
                        m_benchmarkCollectionMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - collectionStart).count();
                    }
                    m_snapshot = m_scene.GetPublishedSceneSnapshot();
                    m_retired = m_scene.TakeRetiredGraphicsResources();
                    m_computeCommands = std::move(computeCommands);
                    m_shadowCommands = std::move(shadowCommands);
                    m_sceneCommands = std::move(sceneCommands);
                }
            }
            catch (...) { failure = std::current_exception(); }
            ::Engine::Core::SetCurrentNativeInputSnapshot(nullptr);
            lock.lock();
            m_failure = failure;
            if (work == Work::Update) m_updateCompleted = true;
            else m_completed = true;
            m_wake.notify_one();
        }
        lock.unlock();
        m_scene.FenceBackgroundWork();
        m_scene.TransferOwnerTo(m_windowThread);
    }

    ::Engine::Scene::Scene& m_scene;
    const std::thread::id m_windowThread;
    const bool m_collectBenchmark;
    GameBenchmarkSceneSample m_benchmarkScene;
    double m_benchmarkCollectionMs = 0;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    ::Engine::Core::NativeInputSnapshot m_input;
    std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> m_snapshot;
    ::Engine::Scene::Scene::RetiredGraphicsResources m_retired;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_computeCommands;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_shadowCommands;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_sceneCommands;
    std::exception_ptr m_failure;
    float m_deltaTime = 0.f;
    std::chrono::steady_clock::time_point m_frameStartedAt;
    TickResult::Timings m_timings;
    uint32_t m_previousOcclusionCulled = 0;
    bool m_ready = false;
    bool m_busy = false;
    Work m_work = Work::None;
    bool m_updateCompleted = false;
    bool m_completed = false;
    bool m_stopping = false;
    std::thread m_worker;
};

// Graphics calls have one owner. The render worker only replays commands and
// retains their resource references through the submitted frame fence.
class RenderLoop
{
public:
    RenderLoop(::Engine::Renderers::IGameRenderer& renderer, std::string scenePath,
        GameBenchmark* gameBenchmark, const GameBenchmarkOptions* benchmarkOptions)
        : m_renderer(renderer), m_scenePath(std::move(scenePath)),
          m_gameBenchmark(gameBenchmark), m_benchmarkOptions(benchmarkOptions)
    {
        char workerFlag[2]{};
        m_workerRecordingEnabled = GetEnvironmentVariableA(
            "ENGINE_WORKER_COMMAND_RECORDING", workerFlag, sizeof(workerFlag)) == 1 &&
            workerFlag[0] == '1';
        m_renderer.SetWorkerRecordingEnabled(
            m_workerRecordingEnabled && m_renderer.SupportsWorkerCommands());
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"ENGINE_COMMAND_RECORD_BENCHMARK", path, 32768);
        if (length && length < 32768)
        {
            m_benchmark.open(std::filesystem::path(path), std::ios::trunc);
            if (m_benchmark)
                m_benchmark << "frame,worker,shadow_ms,frame_ms,latency_ms,backend,scene,gpu,width,height,"
                    "simulation_update_ms,update_handoff_wait_ms,scene_prepare_ms,command_record_ms,"
                    "prepare_handoff_wait_ms,particle_replay_ms,worker_record_ms,shadow_replay_ms,clear_ms,scene_replay_ms,end_frame_ms,queue_submission_ms,"
                    "present_ms,gpu_valid,gpu_stage_sum_ms,gpu_shadow_ms,gpu_terrain_ms,gpu_grass_ms,"
                    "gpu_portal_ms,gpu_opaque_ms,gpu_transparent_ms\n";
        }
        m_worker = std::thread([this] { Run(); });
    }

    ~RenderLoop()
    {
        try { Stop(); }
        catch (...) { /* Destructors cannot report a failed shutdown. */ }
    }
    RenderLoop(const RenderLoop&) = delete;
    RenderLoop& operator=(const RenderLoop&) = delete;

    uint32_t BeginFrame()
    {
        std::lock_guard submitLock(m_submitMutex);
        Submit(Work::BeginFrame, {});
        return m_lastOcclusionCulled.load(std::memory_order_acquire);
    }

    void Render(uint32_t width, uint32_t height,
        SimulationLoop::TickResult tick)
    {
        std::lock_guard submitLock(m_submitMutex);
        Packet packet;
        packet.startedAt = tick.startedAt;
        packet.snapshot = std::move(tick.snapshot);
        packet.retired = std::move(tick.retired);
        packet.computeCommands = std::move(tick.computeCommands);
        packet.shadowCommands = std::move(tick.shadowCommands);
        packet.sceneCommands = std::move(tick.sceneCommands);
        packet.simulationTimings = tick.timings;
        packet.benchmarkScene = std::move(tick.benchmarkScene);
        packet.benchmarkCollectionMs = tick.benchmarkCollectionMs;
        packet.intervalMs = tick.intervalMs;
        packet.residencyWaitMs = tick.residencyWaitMs;
        packet.benchmarkFrame = tick.benchmarkFrame;
        packet.width = width;
        packet.height = height;
        SubmitAsync(Work::Frame, std::move(packet));
    }

    void Resize(uint32_t width, uint32_t height)
    {
        std::lock_guard submitLock(m_submitMutex);
        Packet packet;
        packet.width = width;
        packet.height = height;
        Submit(Work::Resize, std::move(packet));
    }

    void WaitIdle()
    {
        std::lock_guard submitLock(m_submitMutex);
        Submit(Work::Idle, {});
    }

    void Stop()
    {
        std::lock_guard submitLock(m_submitMutex);
        if (!m_worker.joinable()) return;
        std::exception_ptr pendingFailure;
        try { AwaitPending(); }
        catch (...) { pendingFailure = std::current_exception(); }
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_worker.join();
        if (m_failure)
            std::rethrow_exception(m_failure);
        if (pendingFailure)
            std::rethrow_exception(pendingFailure);
    }

private:
    enum class Work { None, BeginFrame, Frame, Resize, Idle };
    struct Packet
    {
        std::chrono::steady_clock::time_point startedAt;
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> computeCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
        SimulationLoop::TickResult::Timings simulationTimings;
        GameBenchmarkSceneSample benchmarkScene;
        double benchmarkCollectionMs = 0, intervalMs = 0, residencyWaitMs = 0;
        uint64_t benchmarkFrame = UINT64_MAX;
        uint32_t width = 0, height = 0;
    };

    void Submit(Work work, Packet packet)
    {
        AwaitPending();
        std::unique_lock lock(m_mutex);
        if (m_stopping)
            throw std::logic_error("Render handoff already has a packet");
        m_packet = std::move(packet);
        m_busy = true;
        m_work = work;
        m_wake.notify_one();
        m_wake.wait(lock, [this] { return m_completed; });
        m_completed = false;
        m_busy = false;
        if (m_failure)
        {
            auto failure = std::exchange(m_failure, nullptr);
            std::rethrow_exception(failure);
        }
    }

    void SubmitAsync(Work work, Packet packet)
    {
        AwaitPending();
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            throw std::logic_error("Render handoff is stopping");
        m_packet = std::move(packet);
        m_busy = true;
        m_work = work;
        m_wake.notify_one();
    }

    void AwaitPending()
    {
        std::unique_lock lock(m_mutex);
        if (!m_busy) return;
        m_wake.wait(lock, [this] { return m_completed; });
        m_completed = false;
        m_busy = false;
        if (m_failure)
        {
            auto failure = std::exchange(m_failure, nullptr);
            std::rethrow_exception(failure);
        }
    }

    void Draw(Packet packet)
    {
        const auto frameStart = std::chrono::steady_clock::now();
        if (!packet.snapshot || !packet.computeCommands ||
            !packet.shadowCommands || !packet.sceneCommands)
            throw std::logic_error("Render handoff has no recorded frame");
        auto graphicsContext = m_renderer.CreateFrameGraphicsContext();
        using FramePlan = ::Engine::Rendering::RenderFramePlan;
        constexpr FramePlan::ResourceId SceneData = 1;
        constexpr FramePlan::ResourceId ParticleState = 2;
        constexpr FramePlan::ResourceId ShadowMap = 3;
        constexpr FramePlan::ResourceId SceneColor = 4;
        constexpr FramePlan::ResourceId SceneDepth = 5;
        constexpr FramePlan::ResourceId SwapChain = 6;
        FramePlan plan;
        double particleReplayMs = 0.0;
        double shadowMs = 0.0;
        bool workerRecorded = false;
        double workerRecordMs = 0.0;
        double shadowReplayMs = 0.0;
        double clearMs = 0.0;
        double sceneReplayMs = 0.0;
        double endFrameMs = 0.0;
        plan.AddPass("Particle simulation",
            {{SceneData, FramePlan::Access::Read},
             {ParticleState, FramePlan::Access::ReadWrite}},
            [&]
            {
                const auto started = std::chrono::steady_clock::now();
                if (graphicsContext) packet.computeCommands->Replay(*graphicsContext);
                particleReplayMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            });
        plan.AddPass("Shadow rendering",
            {{SceneData, FramePlan::Access::Read},
             {ParticleState, FramePlan::Access::Read},
             {ShadowMap, FramePlan::Access::Write}},
            [&]
            {
                const auto shadowStart = std::chrono::steady_clock::now();
                // The shadow stream has its own native allocator/pool. Keep
                // the render owner's frame context untouched while it records.
                if (m_workerRecordingEnabled && m_renderer.SupportsWorkerCommands())
                {
                    const auto workerRecordStart = std::chrono::steady_clock::now();
                    auto shadowRecording = ::Engine::Rendering::RenderJobs::Get().Submit(
                        [this, shadow = packet.shadowCommands]
                        { return m_renderer.RecordWorkerCommands(*shadow); });
                    workerRecorded = shadowRecording.get();
                    workerRecordMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - workerRecordStart).count();
                }
                if (workerRecorded)
                    m_renderer.SubmitWorkerCommands();
                else if (graphicsContext)
                {
                    const auto replayStart = std::chrono::steady_clock::now();
                    packet.shadowCommands->Replay(*graphicsContext);
                    shadowReplayMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - replayStart).count();
                }
                shadowMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - shadowStart).count();
            });
        plan.AddPass("Clear scene color and depth",
            {{SceneColor, FramePlan::Access::Write},
             {SceneDepth, FramePlan::Access::Write}},
            [&]
            {
                const auto started = std::chrono::steady_clock::now();
                m_renderer.Clear(0.1f, 0.1f, 0.1f);
                clearMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            });
        plan.AddPass("Scene color and depth",
            {{SceneData, FramePlan::Access::Read},
             {ParticleState, FramePlan::Access::Read},
             {ShadowMap, FramePlan::Access::Read},
             {SceneColor, FramePlan::Access::Write},
             {SceneDepth, FramePlan::Access::Write}},
            [&]
            {
                const auto started = std::chrono::steady_clock::now();
                if (graphicsContext) packet.sceneCommands->Replay(*graphicsContext);
                sceneReplayMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                m_lastOcclusionCulled.store(
                    packet.sceneCommands->GetReplayOcclusionCulledCount(),
                    std::memory_order_release);
            });
        plan.AddPass("Post-process, composition, and present",
            {{SceneColor, FramePlan::Access::Read},
             {SceneDepth, FramePlan::Access::Read},
             {SwapChain, FramePlan::Access::Write}},
            [&]
            {
                const auto started = std::chrono::steady_clock::now();
                if (m_gameBenchmark && packet.benchmarkFrame ==
                    uint64_t(m_benchmarkOptions->warmup) + m_benchmarkOptions->frames)
                    m_renderer.RequestSceneCapture();
                m_renderer.EndFrame();
                endFrameMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            });
        plan.Execute();
        const double frameMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - frameStart).count();
        const double latencyMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - packet.startedAt).count();
        const auto telemetry = m_renderer.GetFrameTimingTelemetry();
        if (m_gameBenchmark && packet.benchmarkFrame != UINT64_MAX)
        {
            const uint64_t warmup = m_benchmarkOptions->warmup;
            if (packet.benchmarkFrame >= warmup &&
                packet.benchmarkFrame < warmup + m_benchmarkOptions->frames)
            {
                GameBenchmarkFrameSample sample;
                sample.intervalMs = packet.intervalMs;
                sample.updateMs = packet.simulationTimings.simulationUpdateMs;
                sample.prepareMs = packet.simulationTimings.scenePreparationMs;
                sample.recordMs = packet.simulationTimings.commandRecordingMs;
                sample.updateWaitMs = packet.simulationTimings.updateHandoffWaitMs;
                sample.prepareWaitMs = packet.simulationTimings.prepareHandoffWaitMs;
                sample.replayMs = sceneReplayMs;
                sample.endFrameMs = endFrameMs;
                sample.renderThreadMs = frameMs;
                sample.latencyMs = latencyMs;
                sample.metricsCollectionMs = packet.benchmarkCollectionMs;
                sample.residencyWaitMs = packet.residencyWaitMs;
                sample.width = m_renderer.GetWidth();
                sample.height = m_renderer.GetHeight();
                sample.renderScale = m_renderer.GetRenderScale();
                sample.gpu = telemetry;
                sample.scene = std::move(packet.benchmarkScene);
                m_gameBenchmark->Record(packet.benchmarkFrame - warmup, sample);
            }
            else if (packet.benchmarkFrame == warmup + m_benchmarkOptions->frames)
                m_gameBenchmark->Capture(m_renderer);
        }
        double gpuTotalMs = 0.0;
        for (double stageMs : telemetry.gpuMilliseconds) gpuTotalMs += stageMs;
        if (m_benchmark)
            m_benchmark << std::fixed << std::setprecision(4)
                << ++m_benchmarkFrame << ',' << (workerRecorded ? 1 : 0)
                << ',' << shadowMs << ',' << frameMs << ',' << latencyMs
                << ',' << CsvField(m_renderer.GetBackendName())
                << ',' << CsvField(m_scenePath)
                << ',' << CsvField(m_renderer.GetGraphicsDeviceName())
                << ',' << m_renderer.GetWidth() << ',' << m_renderer.GetHeight()
                << ',' << packet.simulationTimings.simulationUpdateMs
                << ',' << packet.simulationTimings.updateHandoffWaitMs
                << ',' << packet.simulationTimings.scenePreparationMs
                << ',' << packet.simulationTimings.commandRecordingMs
                << ',' << packet.simulationTimings.prepareHandoffWaitMs
                << ',' << particleReplayMs
                << ',' << workerRecordMs
                << ',' << shadowReplayMs
                << ',' << clearMs
                << ',' << sceneReplayMs
                << ',' << endFrameMs
                << ',' << m_renderer.GetLastQueueSubmissionMilliseconds()
                << ',' << m_renderer.GetLastPresentationMilliseconds()
                << ',' << (telemetry.gpuTimingsValid ? 1 : 0)
                << ',' << gpuTotalMs
                << ',' << telemetry.gpuMilliseconds[static_cast<size_t>(
                    ::Engine::Graphics::GpuTimingStage::Shadow)]
                << ',' << telemetry.gpuMilliseconds[static_cast<size_t>(
                    ::Engine::Graphics::GpuTimingStage::Terrain)]
                << ',' << telemetry.gpuMilliseconds[static_cast<size_t>(
                    ::Engine::Graphics::GpuTimingStage::Grass)]
                << ',' << telemetry.gpuMilliseconds[static_cast<size_t>(
                    ::Engine::Graphics::GpuTimingStage::Portal)]
                << ',' << telemetry.OpaqueMilliseconds()
                << ',' << telemetry.gpuMilliseconds[static_cast<size_t>(
                    ::Engine::Graphics::GpuTimingStage::Transparent)]
                << '\n';
        const uint64_t token = m_renderer.GetSubmittedFrameToken();
        if (token)
            m_inFlightFrames.push_back(token);
        // The packet owns mesh buffer references used by this submission.
        // Keep the packet and replaced scene resources until the GPU fence.
        m_retiredFrames.push_back({token, std::move(packet.retired),
            packet.computeCommands, packet.shadowCommands, packet.sceneCommands});
        RetireCompletedFrames();
    }

    void RetireCompletedFrames()
    {
        while (!m_inFlightFrames.empty() &&
            m_renderer.IsFrameComplete(m_inFlightFrames.front()))
            m_inFlightFrames.pop_front();
        while (!m_retiredFrames.empty() &&
            m_renderer.IsFrameComplete(m_retiredFrames.front().token))
            m_retiredFrames.pop_front();
        // Bound queued GPU work, including frames with no replaced resources.
        if (m_inFlightFrames.size() >= 3)
        {
            m_renderer.WaitIdle();
            m_inFlightFrames.clear();
            m_retiredFrames.clear();
        }
    }

    void Run()
    {
        std::unique_lock lock(m_mutex);
        while (true)
        {
            m_wake.wait(lock, [this]
            { return m_work != Work::None || m_stopping; });
            if (m_stopping) break;
            const Work work = std::exchange(m_work, Work::None);
            Packet packet = std::move(m_packet);
            lock.unlock();
            std::exception_ptr failure;
            try
            {
                if (work == Work::BeginFrame)
                    m_renderer.BeginFrame();
                else if (work == Work::Resize)
                {
                    m_renderer.WaitIdle();
                    m_inFlightFrames.clear();
                    m_retiredFrames.clear();
                    m_renderer.Resize(packet.width, packet.height);
                }
                else if (work == Work::Idle)
                {
                    m_renderer.WaitIdle();
                    m_inFlightFrames.clear();
                    m_retiredFrames.clear();
                }
                else
                    Draw(std::move(packet));
            }
            catch (...) { failure = std::current_exception(); }
            lock.lock();
            m_failure = failure;
            m_completed = true;
            m_wake.notify_one();
        }
        lock.unlock();
        try
        {
            m_renderer.WaitIdle();
            m_inFlightFrames.clear();
            m_retiredFrames.clear();
        }
        catch (...) { m_failure = std::current_exception(); }
    }

    ::Engine::Renderers::IGameRenderer& m_renderer;
    std::string m_scenePath;
    GameBenchmark* m_gameBenchmark;
    const GameBenchmarkOptions* m_benchmarkOptions;
    bool m_workerRecordingEnabled = false;
    std::ofstream m_benchmark;
    uint64_t m_benchmarkFrame = 0;
    std::atomic<uint32_t> m_lastOcclusionCulled{0};
    std::mutex m_submitMutex;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    Packet m_packet;
    struct RetiredFrame
    {
        uint64_t token = 0;
        ::Engine::Scene::Scene::RetiredGraphicsResources resources;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> computeCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
    };
    std::deque<RetiredFrame> m_retiredFrames;
    std::deque<uint64_t> m_inFlightFrames;
    std::exception_ptr m_failure;
    Work m_work = Work::None;
    bool m_busy = false;
    bool m_completed = false;
    bool m_stopping = false;
    std::thread m_worker;
};
}

int GameApplication::Run(HINSTANCE instance, const GameBenchmarkOptions* benchmark)
{
    const auto startupStart = std::chrono::steady_clock::now();
    RegisterGameComponents();
    Engine::Model::ProjectSettings settings = benchmark && !benchmark->project.empty()
        ? Engine::Core::ProjectLoader{}.LoadProject(benchmark->project.string())
        : LoadGameProjectSettings();
    if (benchmark)
    {
        settings.gameRenderingAPI = benchmark->backend == "dx11" ? "DirectX11" :
            benchmark->backend == "dx12" ? "DirectX12" : "Vulkan";
        settings.viewportWidth = benchmark->width;
        settings.viewportHeight = benchmark->height;
        if (!benchmark->device.empty()) settings.graphicsDevice = benchmark->device;
        if (benchmark->maxFpsOverride) settings.gameMaxFps = *benchmark->maxFpsOverride;
        if (benchmark->disableClusteredLighting) settings.distanceLighting.clusteredLighting = false;
    }
    if (!benchmark && !SelectStartupRenderer(settings))
        return 0;

    const auto startupBackground =
        (std::filesystem::path(settings.assetsDirectory) / "Startup" / "Game.png").wstring();
    ::Engine::Core::StartupSplash splash(instance,
        settings.name.empty() ? L"Game" : std::filesystem::path(settings.name).wstring(),
        startupBackground);
    splash.SetProgress(0.15f, L"Creating game window...");

    auto window = std::make_unique<::Engine::Core::Window>(instance, L"Game",
        settings.viewportWidth, settings.viewportHeight);
    splash.SetProgress(0.35f, L"Initializing renderer...");
    std::unique_ptr<::Engine::Renderers::IGameRenderer> renderer =
        CreateRenderer(settings, *window, benchmark == nullptr);
    if (!renderer)
        return 1;

    splash.SetProgress(0.60f, L"Loading scene...");

    ::Engine::Scene::Scene scene;
    scene.SetDeferredGraphicsRetirement(true);
    scene.Init(renderer->GetGraphicsProvider());
    scene.GetPhysics().ConfigureStepBudget(settings.physicsTargetMilliseconds,
        settings.physicsMaximumSubsteps);
    scene.SetRealtimeShadowSettings(settings.realtimeShadows);
    scene.SetDistanceLightingSettings(settings.distanceLighting);
    Engine::Core::SceneManager::SetActiveScene(&scene);
    const std::string defaultScene = benchmark ? benchmark->scene.string() : settings.defaultScene.empty()
        ? GetFallbackScenePath() : settings.defaultScene;
    Engine::Core::SceneManager::SetDefaultScenePath(defaultScene);
    std::string loadedScenePath = defaultScene;
    if (!scene.Load(loadedScenePath))
    {
        if (benchmark) throw std::runtime_error("could not load benchmark scene: " + loadedScenePath);
        loadedScenePath = GetFallbackScenePath();
        scene.Load(loadedScenePath);
    }

    scene.Start();
    std::unique_ptr<GameBenchmark> gameBenchmark;
    if (benchmark)
        gameBenchmark = std::make_unique<GameBenchmark>(*benchmark, *renderer,
            loadedScenePath, settings.gameMaxFps, settings.distanceLighting.clusteredLighting,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startupStart).count());
    RenderLoop renderLoop(*renderer, loadedScenePath, gameBenchmark.get(), benchmark);
    scene.SetBeforeGpuResourceDestruction([&renderLoop] { renderLoop.WaitIdle(); });
    SimulationLoop simulation(scene, benchmark != nullptr);
    splash.SetProgress(0.90f, L"Starting game...");

    LARGE_INTEGER performanceFrequency{};
    LARGE_INTEGER lastCounter{};
    QueryPerformanceFrequency(&performanceFrequency);
    QueryPerformanceCounter(&lastCounter);
    Engine::Core::FramePacer gameFramePacer;
    bool benchmarkStarted = false;
    bool benchmarkReady = !benchmark || benchmark->residencyTimeoutSeconds == 0;
    auto benchmarkWaitStart = std::chrono::steady_clock::now();
    double residencyWaitMs = 0;
    uint64_t benchmarkFrame = 0;

    window->OnResize = [&renderLoop](uint32_t width, uint32_t height)
    {
        renderLoop.Resize(width, height);
    };
    window->OnUpdate = [&]()
    {
        gameFramePacer.Wait(settings.gameMaxFps);
        LARGE_INTEGER currentCounter{};
        QueryPerformanceCounter(&currentCounter);
        const float deltaTime =
            static_cast<float>(currentCounter.QuadPart - lastCounter.QuadPart) /
            static_cast<float>(performanceFrequency.QuadPart);
        lastCounter = currentCounter;

        const uint32_t width = window->GetWidth();
        const uint32_t height = window->GetHeight();
        auto input = ::Engine::Core::CaptureNativeInput(window->GetHWND());
        if (benchmark && benchmarkStarted && benchmarkReady && benchmark->driveForwardFrames)
        {
            // Feed a reproducible click and W hold through the normal input
            // snapshot path without moving the host cursor or keyboard.
            input.focused = true;
            input.keys.fill(0);
            if (input.hasCenter) input.cursor = input.center;
            const uint64_t clickStart = uint64_t(benchmark->warmup) + 5u;
            if (benchmarkFrame >= clickStart && benchmarkFrame < clickStart + 3u)
                input.keys[VK_LBUTTON] = static_cast<SHORT>(0x8000);
            const uint64_t moveStart = clickStart + 3u;
            if (benchmarkFrame >= moveStart &&
                benchmarkFrame < moveStart + benchmark->driveForwardFrames)
                input.keys['W'] = static_cast<SHORT>(0x8000);
        }
        simulation.BeginUpdate(deltaTime, std::move(input));
        uint32_t previousOcclusionCulled = 0;
        if (width && height)
            previousOcclusionCulled = renderLoop.BeginFrame();
        auto tick = simulation.FinishFrame(width && height,
            previousOcclusionCulled);
        if (benchmarkStarted && width && height)
        {
            if (!benchmarkReady)
            {
                const auto& state = tick.benchmarkScene;
                residencyWaitMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - benchmarkWaitStart).count();
                benchmarkReady = state.camera && state.render.physicalDraws &&
                    state.render.submittedTriangles &&
                    (!benchmark->requireTerrain || state.terrains) &&
                    (!benchmark->requireStreamedCells || state.loadedCells);
                if (!benchmarkReady && residencyWaitMs >=
                    1000.0 * benchmark->residencyTimeoutSeconds)
                {
                    std::cerr << "Timed out waiting for benchmark scene residency\n";
                    PostQuitMessage(2);
                }
            }
            if (benchmarkReady)
            {
                tick.benchmarkFrame = benchmarkFrame++;
                tick.intervalMs = double(deltaTime) * 1000.0;
                tick.residencyWaitMs = residencyWaitMs;
            }
        }
        if (!benchmark || !benchmark->driveForwardFrames)
            ::Engine::Core::ApplyNativeCursorRequests(tick.input);

        if (width && height)
            renderLoop.Render(width, height, std::move(tick));
        if (benchmarkStarted && benchmarkFrame >
            uint64_t(benchmark->warmup) + benchmark->frames)
            PostQuitMessage(0);
    };

    // Keep the native window hidden until it has a rendered frame to show.
    splash.SetProgress(0.95f, L"Drawing game...");
    window->OnUpdate();
    splash.HoldAboveMainWindow();
    window->Show();
    // Hidden swap chains may discard Present, so submit a visible frame too.
    window->OnUpdate();
    splash.Close();
    benchmarkWaitStart = std::chrono::steady_clock::now();
    benchmarkStarted = benchmark != nullptr;
    const int result = window->Run();
    simulation.Stop();
    Engine::Core::SceneManager::CancelPendingSceneLoad();
    scene.FenceBackgroundWork();
    renderLoop.Stop();
    const bool benchmarkValid = !gameBenchmark || gameBenchmark->Finish();
    scene.SetBeforeGpuResourceDestruction({});
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    return benchmarkValid ? result : 2;
}
}
