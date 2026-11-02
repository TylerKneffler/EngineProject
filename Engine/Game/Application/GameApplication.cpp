#include "Game/Application/GameApplication.h"

#include "Game/Startup/GameProjectStartup.h"
#include "Game/Startup/RendererStartup.h"
#include "Core/Window.h"
#include "Core/Startup/StartupSplash.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/SceneManager.h"
#include "Core/Timing/FramePacer.h"
#include "Core/Input/NativeInputSnapshot.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
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
    ::Engine::Core::Window& window)
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
        ::Engine::Core::NativeInputSnapshot input;
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
    };
    explicit SimulationLoop(::Engine::Scene::Scene& scene)
        : m_scene(scene), m_windowThread(std::this_thread::get_id())
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
        m_wake.wait(lock, [this] { return m_updateCompleted; });
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
            return {std::move(m_input), {}, {}, {}, {}};
        }
        m_previousOcclusionCulled = previousOcclusionCulled;
        m_work = Work::Prepare;
        m_wake.notify_one();
        m_wake.wait(lock, [this] { return m_completed; });
        m_completed = false;
        m_busy = false;
        if (m_failure)
        {
            auto failure = std::exchange(m_failure, nullptr);
            std::rethrow_exception(failure);
        }
        return {std::move(m_input), std::move(m_snapshot),
            std::move(m_retired),
            std::move(m_shadowCommands), std::move(m_sceneCommands)};
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
                    m_scene.Update(m_deltaTime);
                    if (::Engine::Core::SceneManager::ProcessPendingSceneLoad() &&
                        m_input.clientWidth && m_input.clientHeight)
                        m_scene.PrepareNativeUiLayout(
                            static_cast<float>(m_input.clientWidth) /
                            static_cast<float>(m_input.clientHeight));
                }
                else
                {
                    m_scene.PrepareRenderFrame();
                    auto drawPacket = m_scene.TakeNativeRenderPacket();
                    auto shadowCommands = std::make_shared<
                        ::Engine::Graphics::GraphicsCommandPacket>();
                    auto sceneCommands = std::make_shared<
                        ::Engine::Graphics::GraphicsCommandPacket>();
                    m_scene.RecordGpuParticleSimulation(shadowCommands.get());
                    if (m_input.clientWidth && m_input.clientHeight)
                        if (auto* gameCamera = m_scene.FindGameCamera())
                        {
                            const float aspect = static_cast<float>(m_input.clientWidth) /
                                static_cast<float>(m_input.clientHeight);
                            m_scene.Render(shadowCommands.get(), aspect,
                                gameCamera, false, m_input.clientWidth,
                                m_input.clientHeight, true, drawPacket.get());
                            m_scene.Render(sceneCommands.get(), aspect,
                                gameCamera, false, 0u, 0u, false,
                                drawPacket.get());
                        }
                    m_scene.RetainNativePacketMeshBuffers(*drawPacket,
                        *shadowCommands);
                    m_scene.SetLastNativeOcclusionCulledCount(
                        m_previousOcclusionCulled);
                    m_snapshot = m_scene.GetPublishedSceneSnapshot();
                    m_retired = m_scene.TakeRetiredGraphicsResources();
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
    std::mutex m_mutex;
    std::condition_variable m_wake;
    ::Engine::Core::NativeInputSnapshot m_input;
    std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> m_snapshot;
    ::Engine::Scene::Scene::RetiredGraphicsResources m_retired;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_shadowCommands;
    std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> m_sceneCommands;
    std::exception_ptr m_failure;
    float m_deltaTime = 0.f;
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
    explicit RenderLoop(::Engine::Renderers::IGameRenderer& renderer)
        : m_renderer(renderer)
    {
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
        packet.snapshot = std::move(tick.snapshot);
        packet.retired = std::move(tick.retired);
        packet.shadowCommands = std::move(tick.shadowCommands);
        packet.sceneCommands = std::move(tick.sceneCommands);
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
        std::shared_ptr<const ::Engine::Scene::Scene::SceneSnapshot> snapshot;
        ::Engine::Scene::Scene::RetiredGraphicsResources retired;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> shadowCommands;
        std::shared_ptr<const ::Engine::Graphics::GraphicsCommandPacket> sceneCommands;
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
        if (!packet.snapshot ||
            !packet.shadowCommands || !packet.sceneCommands)
            throw std::logic_error("Render handoff has no recorded frame");
        auto graphicsContext = m_renderer.CreateFrameGraphicsContext();
        if (graphicsContext)
            packet.shadowCommands->Replay(*graphicsContext);
        m_renderer.Clear(0.1f, 0.1f, 0.1f);
        if (graphicsContext)
            packet.sceneCommands->Replay(*graphicsContext);
        m_lastOcclusionCulled.store(
            packet.sceneCommands->GetReplayOcclusionCulledCount(),
            std::memory_order_release);
        m_renderer.EndFrame();
        const uint64_t token = m_renderer.GetSubmittedFrameToken();
        if (token)
            m_inFlightFrames.push_back(token);
        // The packet owns mesh buffer references used by this submission.
        // Keep the packet and replaced scene resources until the GPU fence.
        m_retiredFrames.push_back({token, std::move(packet.retired),
            packet.shadowCommands, packet.sceneCommands});
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
    std::atomic<uint32_t> m_lastOcclusionCulled{0};
    std::mutex m_submitMutex;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    Packet m_packet;
    struct RetiredFrame
    {
        uint64_t token = 0;
        ::Engine::Scene::Scene::RetiredGraphicsResources resources;
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

int GameApplication::Run(HINSTANCE instance)
{
    RegisterGameComponents();
    Engine::Model::ProjectSettings settings = LoadGameProjectSettings();
    if (!SelectStartupRenderer(settings))
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
    std::unique_ptr<::Engine::Renderers::IGameRenderer> renderer = CreateRenderer(settings, *window);
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
    const std::string defaultScene = settings.defaultScene.empty()
        ? GetFallbackScenePath() : settings.defaultScene;
    Engine::Core::SceneManager::SetDefaultScenePath(defaultScene);
    if (!scene.Load(defaultScene))
        scene.Load(GetFallbackScenePath());

    scene.Start();
    RenderLoop renderLoop(*renderer);
    scene.SetBeforeGpuResourceDestruction([&renderLoop] { renderLoop.WaitIdle(); });
    SimulationLoop simulation(scene);
    splash.SetProgress(0.90f, L"Starting game...");

    LARGE_INTEGER performanceFrequency{};
    LARGE_INTEGER lastCounter{};
    QueryPerformanceFrequency(&performanceFrequency);
    QueryPerformanceCounter(&lastCounter);
    Engine::Core::FramePacer gameFramePacer;

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
        simulation.BeginUpdate(deltaTime,
            ::Engine::Core::CaptureNativeInput(window->GetHWND()));
        uint32_t previousOcclusionCulled = 0;
        if (width && height)
            previousOcclusionCulled = renderLoop.BeginFrame();
        auto tick = simulation.FinishFrame(width && height,
            previousOcclusionCulled);
        ::Engine::Core::ApplyNativeCursorRequests(tick.input);

        if (width && height)
            renderLoop.Render(width, height, std::move(tick));
    };

    // Keep the native window hidden until it has a rendered frame to show.
    splash.SetProgress(0.95f, L"Drawing game...");
    window->OnUpdate();
    splash.HoldAboveMainWindow();
    window->Show();
    // Hidden swap chains may discard Present, so submit a visible frame too.
    window->OnUpdate();
    splash.Close();
    const int result = window->Run();
    simulation.Stop();
    Engine::Core::SceneManager::CancelPendingSceneLoad();
    scene.FenceBackgroundWork();
    renderLoop.Stop();
    scene.SetBeforeGpuResourceDestruction({});
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    return result;
}
}
