#include "Core/Rendering/DetailBudget.h"
#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Runtime/Game.h"
#include "Core/Rendering/AutoQuality.h"
#include "Core/Rendering/HardwareSimulation.h"
#include "Core/Rendering/MaterialCachePolicy.h"
#include "Core/Rendering/RenderingEffects.h"
#include "Game/Application/GameApplication.h"
#include "Core/Startup/WindowPlacementStore.h"

#include "Game/Startup/GameProjectStartup.h"
#include "Game/Startup/RendererStartup.h"
#include "Game/Benchmark/GameBenchmark.h"
#include "Core/ProjectLoader.h"
#include "Core/Window.h"
#include "Core/Startup/StartupSplash.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/UI/Canvas.h"
#include "Core/Components/UI/UIButton.h"
#include "Core/Components/UI/UIObject.h"
#include "Core/Components/UI/UIText.h"
#include "Core/Components/Path/SplineFollower.h"
#include "Core/Components/Path/SplinePath.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Physics/Physics.h"
#include "Core/Time/FramePacer.h"
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
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/Utilities/Rotate.h"
#include "Core/Assets/Scripts/Controllers/FirstPersonController.h"
#include "Core/Assets/Scripts/Gameplay/MainMenuGameManager.h"
#include "Core/Assets/Scripts/Portals/PortalSplitAfterDelay.h"
#include "Core/Assets/Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Serialization/SceneSerializer.h"
#endif

// Benchmark-only scene view render mode for the game camera (-1 = normal).
static int g_benchmarkRenderMode = -1;

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

// GameBenchmarkOptions::route: the SplinePath sampled every Step metres at
// constant speed, so a pose is a lookup (the scene belongs to the simulation
// worker once frames run).
struct BenchmarkRoute
{
    static constexpr float Step = .5f;
    std::vector<glm::dvec3> points;
    float lookAhead = 12.f;

    float Length() const { return points.size() > 1 ? Step * float(points.size() - 1) : 0.f; }
    // Past the end the last segment carries on, so the gaze never collapses
    // onto the eye there.
    glm::dvec3 At(float distance) const
    {
        if (points.size() < 2) return points.empty() ? glm::dvec3(0.0) : points.front();
        const float clamped = std::max(distance, 0.f);
        const size_t last = points.size() - 1;
        const size_t index = std::min(static_cast<size_t>(clamped / Step), last - 1);
        const double t = (double(clamped) - double(index) * Step) / Step;
        return points[index] + (points[index + 1] - points[index]) * t;
    }
    // Eye on the route; looking at the route lookAhead metres on, a little
    // below its eye height (about 3 degrees down at 12 m), so the gaze
    // follows the path's turns and slope as a walker's does.
    ::Engine::Components::Camera::Pose PoseAt(float distance) const
    {
        ::Engine::Components::Camera::Pose pose;
        distance = std::min(distance, Length());
        pose.eye = At(distance);
        pose.target = At(distance + lookAhead) - glm::dvec3(0.0, .05 * lookAhead, 0.0);
        return pose;
    }
};

std::optional<BenchmarkRoute> LoadBenchmarkRoute(const ::Engine::Scene::Scene& scene,
    const GameBenchmarkOptions& options)
{
    if (options.route.empty()) return std::nullopt;
    const auto* object = scene.FindObjectByName(options.route);
    const auto* path = object ? object->GetComponent<SplinePath>() : nullptr;
    if (!path || path->GetControlPointCount() < 2)
        throw std::runtime_error("route not found (needs an object with a SplinePath of two or more points): " +
            options.route);
    BenchmarkRoute route;
    route.lookAhead = std::max(options.routeLookAhead, 1.f);
    const float length = path->GetLength();
    const size_t count = std::max<size_t>(2, static_cast<size_t>(std::ceil(length / BenchmarkRoute::Step)) + 1);
    route.points.reserve(count);
    for (size_t i = 0; i < count; ++i)
        route.points.emplace_back(path->EvaluatePosition(
            std::min(1.f, float(i) * BenchmarkRoute::Step / std::max(length, 1.e-3f))));
    return route;
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

// Flicker flag button (GameBenchmarkOptions::flagButton, or
// ENGINE_FLICKER_BUTTON=1 in normal play): bottom right, in logical units of
// a canvas matched to a 1920 wide reference, so one unit is clientWidth /
// 1920 pixels both ways. The window thread hit-tests the same rectangle.
constexpr float FlagCanvasWidth = 1920.f;
constexpr float FlagButtonWidth = 360.f, FlagButtonHeight = 56.f, FlagButtonMargin = 24.f;
inline bool FlagButtonHit(int x, int y, uint32_t clientWidth, uint32_t clientHeight)
{
    if (!clientWidth || !clientHeight) return false;
    const float scale = static_cast<float>(clientWidth) / FlagCanvasWidth;
    const float right = static_cast<float>(clientWidth) - FlagButtonMargin * scale;
    const float bottom = static_cast<float>(clientHeight) - FlagButtonMargin * scale;
    return x >= right - FlagButtonWidth * scale && x <= right &&
        y >= bottom - FlagButtonHeight * scale && y <= bottom;
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
            double packetPreparationMs = 0.0, shadowRecordingMs = 0.0;
            double cameraRecordingMs = 0.0, resourceRetentionMs = 0.0;
            double prepareHandoffWaitMs = 0.0;
        } timings;
        std::chrono::steady_clock::time_point startedAt;
        ::Engine::Core::NativeInputSnapshot input;
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> computeCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
        // Captured while recording sceneCommands; the render worker composes
        // on another thread, where the thread-local settings are not set.
        ::Engine::Graphics::PostProcessSettings postProcess;
        GameBenchmarkSceneSample benchmarkScene;
        double benchmarkCollectionMs = 0, intervalMs = 0, residencyWaitMs = 0;
        uint64_t benchmarkFrame = UINT64_MAX;
        bool flagged = false; // the flicker flag was pressed this frame
    };
    explicit SimulationLoop(::Engine::Scene::Scene& scene, bool collectBenchmark)
        : m_scene(scene), m_windowThread(std::this_thread::get_id()),
          m_collectBenchmark(collectBenchmark)
    {
        if (collectBenchmark) m_scene.SetUpdateProfilingEnabled(true);
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

    // renderWidth x renderHeight: the renderer's scene size (below the
    // client size with an internal render scale); views record with it.
    // cameraPose: placed on the game camera before the update (benchmark
    // routes); the scene belongs to the worker, so it is applied there.
    void BeginUpdate(float deltaTime,
        ::Engine::Core::NativeInputSnapshot input, uint32_t renderWidth, uint32_t renderHeight,
        std::optional<::Engine::Components::Camera::Pose> cameraPose = std::nullopt)
    {
        std::unique_lock lock(m_mutex);
        if (m_stopping || m_busy)
            throw std::logic_error("Simulation handoff already has a frame");
        m_deltaTime = deltaTime;
        m_cameraPose = cameraPose;
        m_frameStartedAt = std::chrono::steady_clock::now();
        m_timings = {};
        m_input = std::move(input);
        m_renderWidth = renderWidth ? renderWidth : m_input.clientWidth;
        m_renderHeight = renderHeight ? renderHeight : m_input.clientHeight;
        m_busy = true;
        m_work = Work::Update;
        m_wake.notify_one();
    }

    // Flicker flag overlay: shown when enabled, with this label. Window
    // thread, between frames (the worker is idle until BeginUpdate).
    void SetFlagOverlay(bool enabled, std::string label)
    {
        std::lock_guard lock(m_mutex);
        m_flagOverlay = enabled;
        m_flagLabel = std::move(label);
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
        result.postProcess = m_postProcess;
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
                    // Script counters are per frame (the editor resets its own).
                    m_scene.GetCounters().Reset();
                    if (m_cameraPose)
                        if (auto* camera = m_scene.FindGameCamera())
                        {
                            camera->SetWorldPose(*m_cameraPose);
                            // A body (the explorer's) would write its own
                            // position back over the transform.
                            if (auto* body = camera->Owner->GetComponent<::Engine::Components::RigidBody>())
                            {
                                body->SetWorldPosition(glm::vec3(m_cameraPose->eye));
                                body->StopMotion();
                            }
                        }
                    if (auto* game = m_scene.GetGame())
                        game->Update(m_deltaTime);
                    if (m_flagOverlay) UpdateFlagOverlay();
                    if (m_scene.GetGame() && m_scene.GetGame()->ProcessPendingSceneLoad() &&
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
                    // Update already publishes the simulation snapshot. Native
                    // rendering consumes its own owned draw packet, so avoid
                    // two more full-scene snapshot scans during preparation.
                    Engine::Rendering::RenderSystem::For(m_scene).PrepareRenderFrame(false);
                    m_timings.scenePreparationMs = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
                    const auto recordStart = std::chrono::steady_clock::now();
                    auto drawPacket = Engine::Rendering::RenderSystem::For(m_scene).TakeNativeRenderPacket();
                    auto computeCommands = AcquireCommands(0);
                    auto shadowCommands = AcquireCommands(1);
                    auto sceneCommands = AcquireCommands(2);
                    Engine::Rendering::RenderSystem::For(m_scene).RecordGpuParticleSimulation(computeCommands.get());
                    const auto packetPrepared = std::chrono::steady_clock::now();
                    m_timings.packetPreparationMs = std::chrono::duration<double,
                        std::milli>(packetPrepared - recordStart).count();
                    if (m_input.clientWidth && m_input.clientHeight)
                        if (auto* gameCamera = m_scene.FindGameCamera())
                        {
                            const float aspect = static_cast<float>(m_input.clientWidth) /
                                static_cast<float>(m_input.clientHeight);
                            // LOD and texture detail follow the output size
                            // when the renderer upscales.
                            Engine::Rendering::RenderSystem::For(m_scene).SetDetailResolutionScale(
                                m_renderHeight ? static_cast<float>(m_input.clientHeight) /
                                    static_cast<float>(m_renderHeight) : 1.f);
                            Engine::Rendering::RenderSystem::For(m_scene).Render(shadowCommands.get(), aspect,
                                gameCamera, false, m_renderWidth,
                                m_renderHeight, true, drawPacket.get());
                            const auto shadowRecorded = std::chrono::steady_clock::now();
                            m_timings.shadowRecordingMs = std::chrono::duration<double,
                                std::milli>(shadowRecorded - packetPrepared).count();
                            Engine::Rendering::RenderSystem::For(m_scene).Render(sceneCommands.get(), aspect,
                                gameCamera, false, m_renderWidth,
                                m_renderHeight, false,
                                drawPacket.get(), -1, g_benchmarkRenderMode);
                            m_postProcess = ::Engine::Graphics::GetPostProcessSettings();
                            m_timings.cameraRecordingMs = std::chrono::duration<double,
                                std::milli>(std::chrono::steady_clock::now() - shadowRecorded).count();
                        }
                    const auto retainStart = std::chrono::steady_clock::now();
                    Engine::Rendering::RenderSystem::For(m_scene).RetainNativePacketMeshBuffers(*drawPacket,
                        *shadowCommands);
                    Engine::Rendering::RenderSystem::For(m_scene).SetLastNativeOcclusionCulledCount(
                        m_previousOcclusionCulled);
                    m_timings.resourceRetentionMs = std::chrono::duration<double,
                        std::milli>(std::chrono::steady_clock::now() - retainStart).count();
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
                    m_retired = Engine::Rendering::RenderSystem::TakeRetiredGraphicsResources(m_scene);
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
    std::optional<::Engine::Components::Camera::Pose> m_cameraPose;
    bool m_flagOverlay = false;
    std::string m_flagLabel;
    ::Engine::Scene::Scene::ObjectId m_flagButtonId{};
    // The flag button: created in the running scene the first time (and again
    // after a scene load removed it), its label refreshed each frame.
    void UpdateFlagOverlay()
    {
        using namespace ::Engine::Components;
        auto* button = m_flagButtonId ? m_scene.FindObjectById(m_flagButtonId) : nullptr;
        if (!button)
        {
            auto* root = m_scene.AddObject("Flicker flag overlay");
            root->runtimeOnly = true;
            auto* canvas = root->AddComponent<Canvas>();
            canvas->referenceResolution = { FlagCanvasWidth, 1080.f, 0.f };
            canvas->scaleMode = "ScaleWithScreen";
            canvas->matchWidthOrHeight = 1.f;
            canvas->sortingOrder = 1000;
            auto* rootRect = root->AddComponent<UIObject>();
            rootRect->anchorMin = { 0.f, 0.f, 0.f };
            rootRect->anchorMax = { 1.f, 1.f, 0.f };
            rootRect->pivot = { 0.f, 0.f, 0.f };
            rootRect->sizeDelta = { 0.f, 0.f, 0.f };
            button = m_scene.AddObject("Flag flicker button");
            button->runtimeOnly = true;
            m_scene.SetParentLocal(button, root);
            auto* rect = button->AddComponent<UIObject>();
            rect->anchorMin = rect->anchorMax = { 1.f, 1.f, 0.f };
            rect->pivot = { 1.f, 1.f, 0.f };
            rect->anchoredPosition = { -FlagButtonMargin, -FlagButtonMargin, 0.f };
            rect->sizeDelta = { FlagButtonWidth, FlagButtonHeight, 0.f };
            rect->zOrder = 10;
            auto* look = button->AddComponent<UIButton>();
            look->normalColor = { 0.45f, 0.12f, 0.12f };
            look->hoverColor = { 0.60f, 0.18f, 0.18f };
            look->pressedColor = { 0.30f, 0.08f, 0.08f };
            look->alpha = 0.85f;
            auto* text = button->AddComponent<UIText>();
            text->fontSize = 26.f;
            text->horizontalAlignment = "Center";
            text->verticalAlignment = "Center";
            text->overflow = "Visible";
            text->color = { 1.f, 0.95f, 0.9f };
            m_flagButtonId = m_scene.GetObjectId(button);
        }
        if (auto* text = button->GetComponent<UIText>(); text && text->text != m_flagLabel)
            text->text = m_flagLabel;
    }
    uint32_t m_renderWidth = 0, m_renderHeight = 0;
    std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> m_snapshot;
    ::Engine::Scene::Scene::RetiredGraphicsResources m_retired;
    // RenderLoop retains submitted packets through GPU completion. An idle
    // pool entry is therefore safe only when this is its sole remaining owner.
    std::shared_ptr<::Engine::Graphics::GraphicsCommandPacket> AcquireCommands(size_t stream)
    {
        using Commands = ::Engine::Graphics::GraphicsCommandPacket;
        static const bool reuse = [] {
            char value[8]{};
            return GetEnvironmentVariableA("ENGINE_REUSE_GAME_COMMANDS", value, sizeof(value)) == 0 || value[0] != '0';
        }();
        if (!reuse) return std::make_shared<Commands>();
        return m_commandPools[stream].Acquire();
    }
    std::array<::Engine::Graphics::GraphicsCommandPacketPool, 3> m_commandPools;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_computeCommands;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_shadowCommands;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_sceneCommands;
    ::Engine::Graphics::PostProcessSettings m_postProcess;
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
        packet.postProcess = tick.postProcess;
        packet.simulationTimings = tick.timings;
        packet.benchmarkScene = std::move(tick.benchmarkScene);
        packet.benchmarkCollectionMs = tick.benchmarkCollectionMs;
        packet.intervalMs = tick.intervalMs;
        packet.residencyWaitMs = tick.residencyWaitMs;
        packet.benchmarkFrame = tick.benchmarkFrame;
        packet.flagged = tick.flagged;
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

    // Synchronous like Resize: no frame is in flight when the renderer
    // recreates its size-dependent targets.
    void ApplyRenderScale(float scale)
    {
        std::lock_guard submitLock(m_submitMutex);
        Packet packet;
        packet.renderScale = scale;
        Submit(Work::RenderScale, std::move(packet));
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
    enum class Work { None, BeginFrame, Frame, Resize, RenderScale, Idle };
    struct Packet
    {
        std::chrono::steady_clock::time_point startedAt;
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> computeCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
        ::Engine::Graphics::PostProcessSettings postProcess;
        SimulationLoop::TickResult::Timings simulationTimings;
        GameBenchmarkSceneSample benchmarkScene;
        double benchmarkCollectionMs = 0, intervalMs = 0, residencyWaitMs = 0;
        uint64_t benchmarkFrame = UINT64_MAX;
        bool flagged = false;
        uint32_t width = 0, height = 0;
        float renderScale = 1.f; // Work::RenderScale
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
                // Sequence frames go through the async ring with captureAsync
                // (no wait for the GPU); the final capture stays synchronous.
                const bool sequenceFrame = m_gameBenchmark && !MovingCaptureName(packet.benchmarkFrame).empty();
                if (sequenceFrame && m_benchmarkOptions->captureAsync &&
                    m_renderer.RequestAsyncSceneCapture(packet.benchmarkFrame))
                    m_asyncCaptureFrame = packet.benchmarkFrame;
                else if (m_gameBenchmark && (packet.benchmarkFrame ==
                    uint64_t(m_benchmarkOptions->warmup) + m_benchmarkOptions->frames || sequenceFrame))
                    m_renderer.RequestSceneCapture();
                ::Engine::Graphics::SetPostProcessSettings(packet.postProcess);
                m_renderer.EndFrame();
                // ENGINE_GPU_SYNC_EACH_FRAME=1: wait for the GPU after every
                // frame (one frame in flight), to test for races between the
                // CPU preparing a frame and the GPU still running the last.
                static const bool syncEachFrame = [] {
                    const char* value = std::getenv("ENGINE_GPU_SYNC_EACH_FRAME");
                    return value && value[0] == '1'; }();
                if (syncEachFrame) m_renderer.WaitIdle();
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
                sample.packetPrepareMs = packet.simulationTimings.packetPreparationMs;
                sample.shadowRecordMs = packet.simulationTimings.shadowRecordingMs;
                sample.cameraRecordMs = packet.simulationTimings.cameraRecordingMs;
                sample.resourceRetainMs = packet.simulationTimings.resourceRetentionMs;
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
                if (packet.benchmarkFrame == warmup && m_benchmarkOptions->onMeasurementStart)
                    m_benchmarkOptions->onMeasurementStart();
                m_gameBenchmark->Record(packet.benchmarkFrame - warmup, sample);
                if (packet.flagged) m_gameBenchmark->Flag(packet.benchmarkFrame - warmup);
                if (const auto name = MovingCaptureName(packet.benchmarkFrame); !name.empty())
                {
                    if (m_asyncCaptureFrame == packet.benchmarkFrame)
                        m_gameBenchmark->QueueAsyncCapture(packet.benchmarkFrame, name);
                    else
                        m_gameBenchmark->Capture(m_renderer, name.c_str());
                }
                if (m_benchmarkOptions->captureAsync)
                    m_gameBenchmark->PollAsyncCaptures(m_renderer, false);
            }
            else if (packet.benchmarkFrame == warmup + m_benchmarkOptions->frames)
            {
                if (m_benchmarkOptions->onMeasurementEnd)
                    m_benchmarkOptions->onMeasurementEnd();
                if (m_benchmarkOptions->captureAsync)
                    m_gameBenchmark->PollAsyncCaptures(m_renderer, true);
                m_gameBenchmark->Capture(m_renderer);
            }
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
                else if (work == Work::RenderScale)
                {
                    m_renderer.WaitIdle();
                    m_inFlightFrames.clear();
                    m_retiredFrames.clear();
                    m_renderer.ApplyRenderScale(packet.renderScale);
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
    // The sequence frame whose async capture was requested (captureAsync).
    uint64_t m_asyncCaptureFrame = UINT64_MAX;
    // capture_a.ppm / capture_b.ppm for the two moving snapshots (see
    // GameBenchmarkOptions::movingCaptureGap), else null.
    // seq_<n>.ppm for captureBurst consecutive frames from every
    // captureEvery-th frame of the drive.
    std::string MovingCaptureName(uint64_t benchmarkFrame) const
    {
        const auto* options = m_benchmarkOptions;
        if (!options) return {};
        const uint64_t start = uint64_t(options->warmup);
        // Without a drive, over the measured frames (a still camera: flicker).
        const uint64_t span = options->driveForwardFrames ? options->driveForwardFrames : options->frames;
        if (options->captureEvery && benchmarkFrame >= start &&
            benchmarkFrame < start + span &&
            (benchmarkFrame - start) % options->captureEvery <
                std::max<uint64_t>(1u, options->captureBurst))
        {
            char name[32];
            std::snprintf(name, sizeof(name), "seq_%04llu.ppm",
                static_cast<unsigned long long>(benchmarkFrame - start));
            return name;
        }
        if (!options->movingCaptureGap ||
            options->driveForwardFrames < options->movingCaptureGap + 4u) return {};
        const uint64_t second = start + options->driveForwardFrames - 2u;
        if (benchmarkFrame == second) return "capture_b.ppm";
        if (benchmarkFrame + options->movingCaptureGap == second) return "capture_a.ppm";
        return {};
    }
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
    // Render at the display's real pixel size. A DPI-unaware process on a
    // scaled display (125 % here) gets a virtualized, smaller screen: the
    // game drew 1536 x 960 and Windows stretched it to 1920 x 1200, blurring
    // every frame, and captures asking for 1920 x 1080 were clamped.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    // Video profile (Preferences > Video Profiles): --profile, then
    // ENGINE_VIDEO_PROFILE, then the project's active profile. Applied before
    // the window, the renderer and the job workers exist.
    std::optional<Engine::Model::VideoProfile> videoProfile;
    GameBenchmarkOptions profiledBenchmark;
    {
        std::string name = settings.activeVideoProfile;
        if (const char* environment = std::getenv("ENGINE_VIDEO_PROFILE")) name = environment;
        if (benchmark && !benchmark->profile.empty()) name = benchmark->profile;
        if (!name.empty())
        {
            if (const auto* found = Engine::Model::FindVideoProfile(settings.videoProfiles, name))
                videoProfile = *found;
            else if (benchmark)
                throw std::runtime_error("unknown video profile: " + name);
        }
    }
    if (videoProfile)
    {
        if (videoProfile->width && videoProfile->height)
        {
            settings.viewportWidth = videoProfile->width;
            settings.viewportHeight = videoProfile->height;
            if (benchmark)
            {
                // The capture checks it got the size it asked for.
                profiledBenchmark = *benchmark;
                profiledBenchmark.width = videoProfile->width;
                profiledBenchmark.height = videoProfile->height;
                benchmark = &profiledBenchmark;
            }
        }
        if (videoProfile->maxFps >= 0 && !(benchmark && benchmark->maxFpsOverride))
            settings.gameMaxFps = static_cast<uint32_t>(videoProfile->maxFps);
        if (videoProfile->textureBudgetMiB > 0.f)
            settings.detailBudget.textureBudgetMiB = videoProfile->textureBudgetMiB;
        // The job scheduler reads its worker count when first used.
        if (videoProfile->cpuWorkers > 0)
            _putenv_s("ENGINE_WORKER_COUNT", std::to_string(videoProfile->cpuWorkers).c_str());
        Engine::Rendering::HardwareSimulation::Get().Configure(
            videoProfile->cpuLoadMs, videoProfile->gpuLoadMs);
        Engine::Rendering::HardwareSimulation::Get().SetProfileName(videoProfile->name);
    }
    if (!benchmark && !SelectStartupRenderer(settings))
        return 0;

    const auto startupBackground =
        (std::filesystem::path(settings.assetsDirectory) / "Startup" / "Game.png").wstring();
    ::Engine::Core::StartupSplash splash(instance,
        settings.name.empty() ? L"Game" : std::filesystem::path(settings.name).wstring(),
        startupBackground, std::wstring(benchmark ? L"GameCapture" : L"Game"));
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
    Engine::Rendering::RenderSystem::For(scene).SetRealtimeShadowSettings(settings.realtimeShadows);
    Engine::Rendering::RenderSystem::For(scene).SetDistanceLightingSettings(settings.distanceLighting);
    Engine::Rendering::DetailBudget::Get().Configure(settings.layers,
        settings.detailBudget);
    {
        // Diagnostics: ENGINE_MSAA=1|2|4|8, ENGINE_FXAA, ENGINE_AUTO_EXPOSURE and
        // ENGINE_GRASS_MIN_WIDTH=0|1 override the project's rendering effects,
        // e.g. for anti-aliasing A/B captures.
        auto effects = settings.renderingEffects;
        if (const char* value = std::getenv("ENGINE_MSAA"))
            effects.antiAliasing.msaaSamples = static_cast<unsigned>(std::max(1, std::atoi(value)));
        if (const char* value = std::getenv("ENGINE_FXAA"))
            effects.antiAliasing.fxaa = std::atoi(value) != 0;
        if (const char* value = std::getenv("ENGINE_GRASS_MIN_WIDTH"))
            effects.antiAliasing.grassMinimumWidth = std::atoi(value) != 0;
        if (const char* value = std::getenv("ENGINE_TAA"))
            effects.antiAliasing.taa = std::atoi(value) != 0;
        if (const char* value = std::getenv("ENGINE_AUTO_EXPOSURE"))
            effects.postEffects.autoExposure = std::atoi(value) != 0;
        Engine::Rendering::RenderingEffects::Get().Configure(effects);
    }
    // Diagnostics: ENGINE_DETAIL_SCALE pins the global detail scale (the
    // budget stops adapting), e.g. to compare LOD bands in the Detail view.
    if (const char* fixedScale = std::getenv("ENGINE_DETAIL_SCALE"))
        if (const float scale = static_cast<float>(std::atof(fixedScale)); scale > 0.f)
            Engine::Rendering::DetailBudget::Get().SetFixedGlobalScale(scale);
    Engine::Rendering::MaterialCachePolicy::Get().SetProjectSettings(
        settings.materialCache);
    if (benchmark) scene.SetUpdateProfilingEnabled(true);
    if (benchmark) g_benchmarkRenderMode = benchmark->renderMode;
    const std::string defaultScene = benchmark ? benchmark->scene.string() : settings.defaultScene.empty()
        ? GetFallbackScenePath() : settings.defaultScene;
    std::string loadedScenePath = defaultScene;
    if (!scene.Load(loadedScenePath))
    {
        if (benchmark) throw std::runtime_error("could not load benchmark scene: " + loadedScenePath);
        loadedScenePath = GetFallbackScenePath();
        scene.Load(loadedScenePath);
    }

    // Only the editor reads per-object scene snapshot contents.
    scene.SetSceneSnapshotObjectsEnabled(false);
    // Captures pose and drive the player's camera (--camera-position,
    // --viewpoint, --route, --drive-forward). A scene may start the game on a
    // camera that follows a path (landscape demo: MeadowWalkCamera); switch
    // those off so the game camera is the player's again.
    if (benchmark)
        for (const auto& object : scene.GetObjects())
            if (object && object->GetComponent<SplineFollower>())
                if (auto* camera = object->GetComponent<Engine::Components::Camera>())
                    camera->active = false;
    if (benchmark && (benchmark->cameraPosition || benchmark->cameraTarget ||
        !benchmark->viewpoint.empty()))
    {
        auto* camera = scene.FindGameCamera();
        if (!camera || !camera->Owner)
            throw std::runtime_error("benchmark camera pose needs a scene game camera");
        if (!benchmark->viewpoint.empty())
        {
            const auto* viewpointObject = scene.FindObjectByName(benchmark->viewpoint);
            const auto* viewpoint = viewpointObject
                ? viewpointObject->GetComponent<Engine::Components::Camera>() : nullptr;
            if (!viewpoint)
                throw std::runtime_error("viewpoint not found (needs an object with a Camera): " +
                    benchmark->viewpoint);
            // Aim by target point; FirstPersonController keeps aiming at it.
            camera->useTransformRotation = false;
            camera->SetWorldPose(viewpoint->GetWorldPose());
        }
        auto& transform = camera->Owner->transform;
        if (benchmark->cameraPosition)
        {
            const auto& p = *benchmark->cameraPosition;
            transform.position = glm::dvec3(p[0], p[1], p[2]);
            transform.MarkDirty();
        }
        if (benchmark->cameraTarget)
        {
            const auto& t = *benchmark->cameraTarget;
            camera->target = glm::vec3(float(t[0]), float(t[1]), float(t[2]));
            // Aim by target point; FirstPersonController keeps aiming at it.
            camera->useTransformRotation = false;
        }
    }
    // --route: warm up at the route's start; the frame loop walks it.
    const std::optional<BenchmarkRoute> benchmarkRoute = benchmark
        ? LoadBenchmarkRoute(scene, *benchmark) : std::nullopt;
    if (benchmarkRoute)
    {
        auto* camera = scene.FindGameCamera();
        if (!camera || !camera->Owner)
            throw std::runtime_error("benchmark route needs a scene game camera");
        camera->useTransformRotation = false;
        camera->SetWorldPose(benchmarkRoute->PoseAt(0.f));
    }
    if (videoProfile && videoProfile->qualityPreset >= 0)
        scene.settings.graphicsQuality.preset = videoProfile->qualityPreset;
    // Diagnostics: ENGINE_QUALITY_PRESET selects the graphics quality preset
    // (0 Reference .. 3 Low, 4 Auto) without editing the scene, e.g. for
    // captures.
    if (const char* preset = std::getenv("ENGINE_QUALITY_PRESET"))
        scene.settings.graphicsQuality.preset = std::clamp(std::atoi(preset), 0,
            Engine::Model::GraphicsQualitySettings::AutoPreset);
    // Diagnostics: ENGINE_ATMOSPHERE_FOG=0/1 forces the scene height fog off
    // or on (with the scene's or default parameters) for A/B captures.
    if (const char* fog = std::getenv("ENGINE_ATMOSPHERE_FOG"))
        scene.settings.atmosphereFog.enabled = std::atoi(fog) != 0;
    // ENGINE_LIGHT_SHAFTS=0 turns the fog's shadowed in-scatter off; N > 0
    // turns it on with N shadow samples per pixel.
    if (const char* shafts = std::getenv("ENGINE_LIGHT_SHAFTS"))
    {
        const int samples = std::atoi(shafts);
        scene.settings.atmosphereFog.lightShafts = samples > 0;
        if (samples > 0)
            scene.settings.atmosphereFog.lightShaftSamples = std::min(samples,
                Engine::Model::AtmosphereFogSettings::MaximumLightShaftSamples);
    }
    Engine::Runtime::Game game(scene);
    game.SetDefaultScenePath(defaultScene);
    game.Start();
    // Internal render scale: the scene's render scale times the quality
    // preset's (as the editor's game view applies them), or
    // ENGINE_RENDER_SCALE (0.5-1) for A/B runs. With TAA the post-process
    // upscales temporally, otherwise bilinearly. Steps of 1/100 keep small
    // drifts from recreating targets. Applied here before the first frame
    // (and before the benchmark records the renderer's settings), then
    // between frames whenever it changes.
    float renderScaleOverride = 0.f;
    if (const char* value = std::getenv("ENGINE_RENDER_SCALE"))
        if (const float scale = static_cast<float>(std::atof(value)); scale > 0.f)
            renderScaleOverride = std::clamp(scale, .5f, 1.f);
    const auto desiredRenderScale = [&]
    {
        const float scale = renderScaleOverride > 0.f ? renderScaleOverride
            : std::clamp(scene.settings.renderScale *
                scene.settings.graphicsQuality.Resolve().renderScale, .5f, 1.f);
        return std::round(scale * 100.f) / 100.f;
    };
    float appliedRenderScale = desiredRenderScale();
    renderer->ApplyRenderScale(appliedRenderScale);
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
    // Flicker flag button state (window thread).
    const bool flagButton = benchmark ? benchmark->flagButton : [] {
        const char* value = std::getenv("ENGINE_FLICKER_BUTTON");
        return value && value[0] == '1'; }();
    bool flagKeyWasDown = false, flagClickWasDown = false;
    uint32_t flagCount = 0;
    uint64_t gameFrameCounter = 0;
    std::string flagLabel = "Flag flicker (F8)";
    auto flagLabelUntil = std::chrono::steady_clock::now();

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
        // Auto quality holds the frame-time target: the FPS limit when one is
        // set, otherwise the scene's render-scale target. The frame's work
        // time leaves out the limiter's sleep.
        {
            const double targetMs = settings.gameMaxFps > 0
                ? 1000.0 / settings.gameMaxFps : static_cast<double>(scene.settings.renderScaleTargetMs);
            const double workMs = std::max(0.0, (static_cast<double>(deltaTime) - gameFramePacer.LastWaitSeconds()) * 1000.0);
            Engine::Rendering::AutoQuality::Get().Update(
                scene.settings.graphicsQuality.preset == Engine::Model::GraphicsQualitySettings::AutoPreset,
                workMs, targetMs, deltaTime);
        }
        // Video profile hardware simulation: a slower CPU's extra frame time.
        Engine::Rendering::HardwareSimulation::Get().SpendCpu();

        const uint32_t width = window->GetWidth();
        const uint32_t height = window->GetHeight();
        // Flicker flag (F8 or the button): read straight from the window, as
        // benchmark routes replace the input snapshot with scripted input.
        bool flagPressed = false;
        if (flagButton)
        {
            const HWND hwnd = window->GetHWND();
            const bool focused = GetForegroundWindow() == hwnd;
            const bool f8 = focused && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            bool click = false;
            if (focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
            {
                POINT cursor{};
                if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
                    click = FlagButtonHit(cursor.x, cursor.y, width, height);
            }
            flagPressed = (f8 && !flagKeyWasDown) || (click && !flagClickWasDown);
            flagKeyWasDown = f8;
            flagClickWasDown = click;
            if (flagPressed)
            {
                ++flagCount;
                const auto now = std::chrono::system_clock::now();
                const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                if (gameBenchmark && benchmarkReady)
                    std::cout << "[flag] #" << flagCount << " at measured frame "
                        << (benchmarkFrame >= benchmark->warmup ? benchmarkFrame - benchmark->warmup : 0)
                        << std::endl;
                else
                {
                    std::filesystem::create_directories(".local");
                    std::ofstream(".local/flicker-flags.txt", std::ios::app)
                        << "flag " << flagCount << " frame " << gameFrameCounter
                        << " unix_ms " << stamp << '\n';
                }
                flagLabel = "Flagged #" + std::to_string(flagCount) + "  (F8 again)";
                flagLabelUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            }
            else if (std::chrono::steady_clock::now() > flagLabelUntil)
                flagLabel = flagCount ? "Flag flicker (F8)  -  " + std::to_string(flagCount) + " so far"
                    : "Flag flicker (F8)";
            simulation.SetFlagOverlay(true, flagLabel);
        }
        ++gameFrameCounter;
        auto input = ::Engine::Core::CaptureNativeInput(window->GetHWND());
        // Benchmarks never see the host's mouse and keyboard: a capture window
        // that ended up in the foreground let the user's mouse turn the
        // camera during warmup, so runs of the same viewpoint diverged. Only
        // the synthetic input below drives them.
        if (benchmark)
        {
            input.focused = false;
            input.keys.fill(0);
            if (input.hasCenter) input.cursor = input.center;
        }
        // A route walk moves the camera itself, from the frame W would go
        // down, routeSpeed / 60 metres per frame (see GameBenchmarkOptions).
        std::optional<::Engine::Components::Camera::Pose> routePose;
        if (benchmarkRoute)
        {
            const uint64_t moveStart = uint64_t(benchmark->warmup) + 8u;
            const uint64_t walked = benchmarkStarted && benchmarkReady && benchmarkFrame > moveStart
                ? std::min<uint64_t>(benchmarkFrame - moveStart,
                    benchmark->driveForwardFrames ? benchmark->driveForwardFrames : UINT64_MAX) : 0u;
            routePose = benchmarkRoute->PoseAt(float(walked) * benchmark->routeSpeed / 60.f);
            if (benchmark->routeLookSway != 0.f)
            {
                const double angle = glm::radians(double(benchmark->routeLookSway)) *
                    std::sin(double(walked) * 2.0 * 3.14159265358979 /
                        double(std::max(8u, benchmark->routeLookPeriod)));
                const glm::dvec3 gaze = routePose->target - routePose->eye;
                const double c = std::cos(angle), s = std::sin(angle);
                routePose->target = routePose->eye +
                    glm::dvec3(c * gaze.x + s * gaze.z, gaze.y, -s * gaze.x + c * gaze.z);
            }
        }
        if (benchmark && !benchmarkRoute && benchmarkStarted && benchmarkReady &&
            (benchmark->driveForwardFrames || benchmark->lookYawPixels))
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
            if (benchmark->lookYawPixels && benchmarkFrame >= moveStart && input.hasCenter)
                input.cursor.x = input.center.x + benchmark->lookYawPixels;
            if (benchmarkFrame >= moveStart &&
                benchmarkFrame < moveStart + benchmark->driveForwardFrames)
            {
                input.keys['W'] = static_cast<SHORT>(0x8000);
                if (benchmark->driveBoost)
                {
                    input.keys[VK_CONTROL] = static_cast<SHORT>(0x8000);
                    input.keys[VK_LCONTROL] = static_cast<SHORT>(0x8000);
                }
            }
        }
        if (const float scale = desiredRenderScale(); scale != appliedRenderScale)
        {
            renderLoop.ApplyRenderScale(scale);
            appliedRenderScale = scale;
        }
        simulation.BeginUpdate(deltaTime, std::move(input),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(), routePose);
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
                    (!benchmark->requireStreamedCells || state.loadedCells) &&
                    state.farObjectsSettled;
                if (!benchmarkReady && residencyWaitMs >=
                    1000.0 * benchmark->residencyTimeoutSeconds)
                {
                    std::cerr << "Timed out waiting for benchmark scene residency (camera "
                        << state.camera << ", draws " << state.render.physicalDraws
                        << ", terrains " << state.terrains << ", loaded cells " << state.loadedCells
                        << ", far objects settled " << state.farObjectsSettled
                        << ": cells " << state.farObjectCells << ", queued " << state.farObjectQueuedCells
                        << ", blocks " << state.farObjectBlocks << ", preparing "
                        << state.farObjectPreparingBlocks << ")\n";
                    PostQuitMessage(2);
                }
            }
            if (benchmarkReady)
            {
                tick.flagged = flagPressed;
                tick.benchmarkFrame = benchmarkFrame++;
                tick.intervalMs = double(deltaTime) * 1000.0;
                tick.residencyWaitMs = residencyWaitMs;
            }
        }
        if (!benchmark || (!benchmark->driveForwardFrames && !benchmark->lookYawPixels))
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
    // Reopen where the game was last left (keeping the configured size), or
    // where the splash was dragged to. Benchmarks keep no placement and are
    // never moved: placement clamps to the monitor's work area, which shrank
    // a 1920x1080 capture to 1520x873 when the splash sat on a smaller
    // (or DPI-scaled) monitor. Parts off screen still render.
    const int showCommand = benchmark ? SW_SHOWDEFAULT
        : splash.PlaceWindow(window->GetHWND(), false);
    // Benchmark and capture windows remember only where they were left (their
    // own key, so they do not disturb the game's): moved there by the top-left
    // corner, never resized or clamped, for the reason above.
    if (benchmark)
        if (const auto saved = ::Engine::Core::LoadWindowPlacement(L"GameCapture"))
            SetWindowPos(window->GetHWND(), nullptr, saved->bounds.left, saved->bounds.top, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    window->SetPlacementKey(benchmark ? L"GameCapture" : L"Game");
    splash.HoldAboveMainWindow();
    window->Show(showCommand);
    // Hidden swap chains may discard Present, so submit a visible frame too.
    window->OnUpdate();
    splash.Close();
    benchmarkWaitStart = std::chrono::steady_clock::now();
    benchmarkStarted = benchmark != nullptr;
    const int result = window->Run();
    // Shutdown can take seconds without pumping messages; hide the window
    // so Windows does not ghost it as "Not Responding" meanwhile.
    ShowWindow(window->GetHWND(), SW_HIDE);
    simulation.Stop();
    if (auto* game = scene.GetGame()) game->CancelPendingSceneLoad();
    scene.FenceBackgroundWork();
    renderLoop.Stop();
    const bool benchmarkValid = !gameBenchmark || gameBenchmark->Finish();
    scene.SetBeforeGpuResourceDestruction({});
    return benchmarkValid ? result : 2;
}
}
