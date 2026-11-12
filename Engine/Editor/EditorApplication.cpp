#include "Engine/Editor/EditorApplication.h"
#include "Engine/Editor/Runtime/EditorPlayPipeline.h"
#include "Engine/Editor/Startup/EditorCommandLine.h"
#include "Engine/Editor/Startup/EditorStartupSettings.h"
#include "Engine/Editor/Startup/EditorSceneSession.h"
#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/Window.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Startup/StartupSplash.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Assets/Scripts/Grass/GrassField.h"
#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Timing/FramePacer.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/RenderFramePlan.h"
#include "Core/View/View.h"
#include "Engine/Editor/State/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/HotReload/EditorHotReload.h"
#include "Engine/Editor/ProjectLauncher.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Engine/Editor/Core/View/Views/GameView.h"
#include "Engine/Editor/Core/View/Views/PerformanceView.h"
#include "Engine/Editor/UI/IEditorUiBackend.h"
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/Utilities/Rotate.h"
#include "Core/Assets/Scripts/Controllers/FirstPersonController.h"
#include "Core/Assets/Scripts/Portals/PortalSplitAfterDelay.h"
#include "Core/Assets/Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Serialization/SceneSerializer.h"
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unordered_set>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
#endif
#ifndef PROJECT_SCRIPTS_PATH
#define PROJECT_SCRIPTS_PATH "build/Lib/Debug/Lib.dll"
#endif

namespace
{
    std::optional<std::string> EnvironmentVariable(const char* name)
    {
#ifdef _MSC_VER
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || !value)
            return std::nullopt;
        std::string result(value);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? std::optional<std::string>(value) : std::nullopt;
#endif
    }

    void WriteStartupLog(const std::string& message, bool reset = false)
    {
        std::ofstream log("editor-startup.log", reset ? std::ios::trunc : std::ios::app);
        if (log)
            log << message << '\n';
    }
}

int Engine::Editor::EditorApplication::Run(HINSTANCE hInstance)
{
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
    Engine::Serialization::RegisterComponentType<Rotate>("Rotate");
    Engine::Serialization::RegisterComponentType<FirstPersonController>("FirstPersonController");
    Engine::Serialization::RegisterComponentType<PortalSplitAfterDelay>(
        "PortalSplitAfterDelay");
    Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
#endif
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WriteStartupLog("Editor startup", true);

    const Engine::Editor::EditorCommandLine commandLine;
    if (const auto importResult = commandLine.RunImportCommand())
    {
        if (SUCCEEDED(comResult)) CoUninitialize();
        return *importResult;
    }

    std::string projectFile = commandLine.ResolveProject();
    if (commandLine.HasArgument(L"--project-hub"))
        projectFile.clear();
    Engine::Core::ProjectLoader projectLoader;
    Engine::Model::ProjectSettings projectSettings;
    const bool engineDevelopmentMode = projectFile.empty() &&
        !commandLine.HasArgument(L"--project-hub") &&
        Engine::Editor::IsEngineDevelopmentDirectory();
    if (engineDevelopmentMode)
    {
        projectSettings = Engine::Editor::CreateEngineDevelopmentSettings();
        const std::string overrideApi = commandLine.RendererOverride();
        if (!overrideApi.empty())
            projectSettings.editorRenderingAPI = overrideApi;
        WriteStartupLog("Mode: Engine Sandbox");
        WriteStartupLog("Editor renderer: " + projectSettings.editorRenderingAPI);
    }
    else
    {
        while (true)
        {
            if (projectFile.empty())
                projectFile = Engine::Editor::ProjectLauncher::Run(hInstance);
            if (projectFile.empty())
            {
                if (SUCCEEDED(comResult)) CoUninitialize();
                return 0;
            }

            try
            {
                projectFile = std::filesystem::weakly_canonical(projectFile).string();
                std::filesystem::current_path(std::filesystem::path(projectFile).parent_path());
                projectSettings = projectLoader.LoadProject(projectFile);
                const std::string overrideApi = commandLine.RendererOverride();
                if (!overrideApi.empty())
                    projectSettings.editorRenderingAPI = overrideApi;
                WriteStartupLog("Project: " + projectFile);
                WriteStartupLog("Editor renderer: " + projectSettings.editorRenderingAPI);
                Engine::Editor::ProjectLauncher::RememberProject(projectFile);
                break;
            }
            catch (const std::exception& error)
            {
                MessageBoxA(nullptr, error.what(), "Could not open project", MB_OK | MB_ICONERROR);
                projectFile.clear();
            }
        }
    }

    const bool standardBenchmark =
        EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_SCENE").has_value() ||
        EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_MMD").has_value();
    if (standardBenchmark)
    {
        const auto benchmarkWidth = EnvironmentVariable("ENGINE_EDITOR_BENCHMARK_WIDTH");
        const auto benchmarkHeight = EnvironmentVariable("ENGINE_EDITOR_BENCHMARK_HEIGHT");
        const uint32_t width = benchmarkWidth
            ? static_cast<uint32_t>(std::clamp(std::atoi(benchmarkWidth->c_str()), 320, 7680))
            : 1920u;
        const uint32_t height = benchmarkHeight
            ? static_cast<uint32_t>(std::clamp(std::atoi(benchmarkHeight->c_str()), 200, 4320))
            : 1080u;
        projectSettings.viewportWidth = width;
        projectSettings.viewportHeight = height;
        projectSettings.aspectRatioMode = Engine::Model::ProjectSettings::AspectRatioMode::Locked;
        projectSettings.gameAspectRatio = static_cast<float>(width) / height;
    }

    const auto startupBackground =
        (std::filesystem::path(projectSettings.assetsDirectory) / "Startup" / "Editor.png").wstring();
    Engine::Core::StartupSplash splash(hInstance, L"Engine Editor", startupBackground);
    splash.SetProgress(0.10f, L"Creating editor window...");

    // Create editor state and UI
    OutputDebugStringA("[Main] Creating EditorState...\n");
    auto editorState = std::make_unique<Engine::Editor::EditorState>(
        hInstance, projectSettings, projectFile);
    splash.SetProgress(0.25f, L"Initializing renderer and scene...");
    OutputDebugStringA("[Main] EditorState created, calling Init...\n");
    if (!editorState->Init())
    {
        WriteStartupLog("Editor initialization failed with " + projectSettings.editorRenderingAPI);
        std::string fallbackReason;
        if (projectSettings.editorRenderingAPI != "DirectX11" &&
            ::Engine::Renderers::RendererFactory::IsRendererAvailable("DirectX11", &fallbackReason))
        {
            OutputDebugStringA(("[Main] " + projectSettings.editorRenderingAPI +
                " editor initialization failed; retrying with DirectX11.\n").c_str());
            projectSettings.editorRenderingAPI = "DirectX11";
            editorState = std::make_unique<Engine::Editor::EditorState>(
                hInstance, projectSettings, projectFile);
            if (!editorState->Init())
            {
                WriteStartupLog("DirectX11 fallback initialization failed");
                return 1;
            }
        }
        else
            return 1;
    }
    WriteStartupLog("Editor initialized successfully");
    splash.SetProgress(0.55f, L"Preparing editor interface...");
    OutputDebugStringA("[Main] EditorState initialized\n");

    Engine::Core::Window* window = editorState->GetWindow();
    Engine::Renderers::IEditorRenderer* renderer = editorState->GetRenderer();
    Engine::Scene::Scene* scene = editorState->GetScene();
    const bool sizeDx11ViewsToPanels =
        dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(renderer) != nullptr &&
        !EnvironmentVariable("ENGINE_EDITOR_DISABLE_PANEL_SIZING").has_value();
    if (!window || !renderer || !scene)
    {
        WriteStartupLog("Editor did not create all required core components");
        return 1;
    }
    Engine::Core::SceneManager::SetActiveScene(scene);
    scene->SetBeforeGpuResourceDestruction([renderer] { renderer->WaitIdle(); });
    scene->SetDeferredGraphicsRetirement(true);
    scene->SetGameplayInputEnabled(false);
    Engine::Core::SceneManager::SetDefaultScenePath(projectSettings.defaultScene);

    auto uiBackend = Engine::Editor::CreateEditorUiBackend();
    if (!uiBackend->Initialize(window->GetHWND(), *renderer))
    {
        WriteStartupLog("UI backend initialization failed");
        return 1;
    }
    renderer->SetUiRenderHooks({
        [&]() { uiBackend->BeginFrame(); },
        [&](void* commands) { uiBackend->Render(commands); },
        [&]() { uiBackend->EndFrame(); }
    });
    editorState->InitializeUiState();
    if (standardBenchmark)
    {
        const auto benchmarkWidth = EnvironmentVariable("ENGINE_EDITOR_BENCHMARK_WIDTH");
        const auto benchmarkHeight = EnvironmentVariable("ENGINE_EDITOR_BENCHMARK_HEIGHT");
        const uint32_t width = benchmarkWidth
            ? static_cast<uint32_t>(std::clamp(std::atoi(benchmarkWidth->c_str()), 320, 7680))
            : 1920u;
        const uint32_t height = benchmarkHeight
            ? static_cast<uint32_t>(std::clamp(std::atoi(benchmarkHeight->c_str()), 200, 4320))
            : 1080u;
        for (const auto& panel : editorState->GetPanels())
            if (auto* view = dynamic_cast<Engine::Editor::View*>(panel.get()))
                view->SetFixedRenderSize(renderer->GetNativeDeviceHandle(), width, height);
    }
    if (const auto benchmarkViews = EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW");
        benchmarkViews.has_value())
        for (const auto& panel : editorState->GetPanels())
        {
            if (*benchmarkViews == "scene")
            {
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(panel.get()))
                    sceneView->SetOpen(false);
            }
            if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(panel.get()))
                gameView->SetOpen(true);
        }
    splash.SetProgress(0.75f, L"Loading startup scene...");

    // The opt-in playback audit runs a selected scene through the complete
    // editor and UI frame loop. The MMD scene remains the default audit scene.
    const auto requestedProfileScene =
        EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_SCENE");
    const bool autoProfilePlay = requestedProfileScene.has_value() ||
        EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_MMD").has_value();
    const auto requestedAutoProfileFrames =
        EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_FRAMES");
    const uint32_t autoProfilePlayFrames = requestedAutoProfileFrames
        ? static_cast<uint32_t>(std::clamp(
            std::atoi(requestedAutoProfileFrames->c_str()), 30, 1200))
        : 180u;
    const std::string startupScene = requestedProfileScene
        ? *requestedProfileScene
        : autoProfilePlay
            ? (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes" /
                "Animation" / "mmd.scene").string()
            : projectSettings.defaultScene;
    if (!startupScene.empty())
    {
        OutputDebugStringA(("[Main] Loading startup scene: " + startupScene + "\n").c_str());
        editorState->LoadStartupScene(startupScene, !autoProfilePlay);
    }
    // Restore the tabs left open at shutdown. Explicitly closed animation
    // documents are removed from the saved session by their close handler.
    editorState->RestoreAnimationEditorSession();
    splash.SetProgress(0.90f, L"Finishing startup...");

    OutputDebugStringA("[Main] Creating GameBuildManager...\n");
    auto gameBuildManager = std::make_unique<Engine::Editor::GameBuildManager>(
        editorState->GetConsole(), projectFile, projectSettings);
    auto hotReload = std::make_unique<Engine::Editor::EditorHotReload>(*editorState,
        editorState->GetConsole(), projectSettings.scriptsDirectory,
        ENGINE_BUILD_DIR, PROJECT_SCRIPTS_PATH);
    const auto pipelineOption = EnvironmentVariable("ENGINE_EDITOR_PLAY_PIPELINE");
    auto playPipeline = renderer->SupportsFrameCompletion() &&
        (!pipelineOption || *pipelineOption != "0")
        ? std::make_unique<Engine::Editor::EditorPlayPipeline>(*renderer) : nullptr;
    bool pendingPlayStopRestore = false;
    bool pendingPlayStart = false;
    unsigned playStartFollowupUpdates = 0;
    unsigned playStopFollowupUpdates = 0;
    bool resetPlayDelta = false;
    const auto finishPlayStop = [&]()
    {
        if (!pendingPlayStopRestore) return;
        Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStop);
        playStopFollowupUpdates = 2;
        using StopProfile = Engine::Serialization::PrefabLoadProfile;
        StopProfile profile("Editor: Stop restoration");
        pendingPlayStopRestore = false;
        {
        StopProfile::Stage stage("stop_drain_pipeline");
        if (playPipeline) playPipeline->Drain();
        }
        {
        StopProfile::Stage stage("stop_cancel_and_fence");
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        scene->FenceBackgroundWork();
        Engine::Core::SceneManager::SetOnSceneLoaded({});
        }
        {
        StopProfile::Stage stage("stop_restore_editor_scene");
        editorState->RestorePlayModeScene();
        }
        scene = editorState->GetScene();
    };
    hotReload->BeforeApply = [&]()
    {
        gameBuildManager->Stop();
        finishPlayStop();
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        scene->FenceBackgroundWork();
        Engine::Core::SceneManager::SetOnSceneLoaded({});
    };
    OutputDebugStringA("[Main] GameBuildManager created\n");
    OutputDebugStringA("[Main] Getting window, renderer, scene...\n");
    char buf[256];
    sprintf_s(buf, "[Main] Window pointer: %p\n", (void*)window);
    OutputDebugStringA(buf);
    sprintf_s(buf, "[Main] Renderer pointer: %p\n", (void*)renderer);
    OutputDebugStringA(buf);
    sprintf_s(buf, "[Main] Scene pointer: %p\n", (void*)scene);
    OutputDebugStringA(buf);
    
    OutputDebugStringA("[Main] Got editor components\n");

    bool waitingForPlayDebugger = false;
    std::chrono::steady_clock::time_point debuggerDeadline;
    const auto beginPlayScene = [&]()
    {
        Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStart);
        playStartFollowupUpdates = 2;
        pendingPlayStart = false;
        scene->Start();
        resetPlayDelta = true;
    };
    gameBuildManager->OnPlayStart = [&]()
    {
        Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStart);
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        scene->FenceBackgroundWork();
        scene->SetGameplayInputEnabled(false);
        const bool requested = editorState->RequestCodeEditorAttach();
        if (!editorState->CapturePlayModeScene())
        {
            gameBuildManager->Stop();
            return;
        }
        pendingPlayStart = true;
        waitingForPlayDebugger = requested && !IsDebuggerPresent();
        if (waitingForPlayDebugger)
        {
            debuggerDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(4);
            if (auto* console = editorState->GetConsole())
                console->AddLog(Engine::Editor::ConsoleView::Level::Info,
                    "[Debug] Waiting up to 4 seconds for VS Code to attach.");
        }
    };
    gameBuildManager->OnPlayStop = [&]()
    {
        Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStop);
        scene->SetGameplayInputEnabled(false);
        window->ReleaseGameplayInput();
        waitingForPlayDebugger = false;
        pendingPlayStart = false;
        playStartFollowupUpdates = 0;
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        // A toolbar Stop can arrive while RenderIfNeeded still owns a command
        // list. Restore once that frame has been submitted.
        pendingPlayStopRestore = true;
    };
    gameBuildManager->OnBuildComplete = [&](bool)
    {
        editorState->CheckAssetLocations();
        renderer->MarkDirty();
    };
    editorState->OnSceneLoadRequested = [&](const std::string&)
    {
        gameBuildManager->Stop();
        finishPlayStop();
    };

    // Frame timing
    OutputDebugStringA("[Main] Setting up frame timing...\n");
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);
    Engine::Core::FramePacer editorFramePacer;

    // Play state
    Engine::Editor::PlayState playState = Engine::Editor::PlayState::Stopped;
    OutputDebugStringA("[Main] Frame timing setup complete\n");

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    OutputDebugStringA("[Main] Setting up window callbacks...\n");
    
    OutputDebugStringA("[Main] Setting OnResize callback...\n");
    bool submittingEditorFrame = false;
    bool deferredClearGameplayFocus = false, deferredAssetCheck = false;
    std::optional<std::pair<uint32_t, uint32_t>> deferredResize;
    const auto resizeEditor = [&](uint32_t w, uint32_t h)
    {
        char buf[256];
        sprintf_s(buf, "[OnResize] Called with w=%u, h=%u\n", w, h);
        OutputDebugStringA(buf);
        
        renderer->Resize(w, h);
        uiBackend->Resize(w, h);
        if (!sizeDx11ViewsToPanels)
            for (auto& panel : editorState->GetPanels())
            {
                if (panel->NeedsRender())
                {
                    Engine::Editor::View* view = reinterpret_cast<Engine::Editor::View*>(panel.get());
                    view->Resize(renderer->GetNativeDeviceHandle(), w, h);
                }
            }
        renderer->MarkDirty();
    };
    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        // DXGI can deliver window messages reentrantly during submission.
        if (submittingEditorFrame || (playPipeline && playPipeline->Busy())) { deferredResize = {w, h}; return; }
        if (playPipeline) playPipeline->Drain();
        resizeEditor(w, h);
    };
    OutputDebugStringA("[Main] OnResize callback set\n");

    OutputDebugStringA("[Main] Setting WndProcHook callback...\n");
    window->WndProcHook = [&](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        if (Engine::Core::Window::MessageRequestsRedraw(m))
            renderer->MarkDirty();
        return uiBackend->HandleMessage(h, m, w, l);
    };
    window->OnInputBegin = [&]() { uiBackend->BeginInput(); };
    window->OnInputEnd = [&]() { uiBackend->EndInput(); };
    window->OnEscapePressed = [&]()
    {
        if (submittingEditorFrame || (playPipeline && playPipeline->Busy())) { deferredClearGameplayFocus = true; return; }
        scene->SetGameplayInputEnabled(false);
        uiBackend->ClearFocus();
        renderer->MarkDirty();
    };
    window->OnApplicationFocusLost = [&]()
    {
        if (submittingEditorFrame || (playPipeline && playPipeline->Busy())) { deferredClearGameplayFocus = true; return; }
        scene->SetGameplayInputEnabled(false);
        uiBackend->ClearFocus();
        renderer->MarkDirty();
    };
    window->OnApplicationFocusGained = [&]()
    {
        if (submittingEditorFrame || (playPipeline && playPipeline->Busy())) { deferredAssetCheck = true; return; }
        editorState->CheckAssetLocations();
        renderer->MarkDirty();
    };
    OutputDebugStringA("[Main] WndProcHook callback set\n");

    OutputDebugStringA("[Main] Setting OnUpdate callback...\n");
    std::string displayedWindowTitle;
    Engine::Editor::EditorFrameProfiler frameProfiler(
        scene, renderer, autoProfilePlay);
    uint32_t autoProfileTick = 0,autoProfilePlayingFrames=0;
    uint64_t recordedFrameCount = 0;
    std::chrono::steady_clock::time_point previousUpdateBegin{}, previousSubmissionSample{};
    double previousIdleWaitMs = 0;
    const bool autoProfileKeepOpen=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_KEEP_OPEN").has_value();
    const auto warmupFrames=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES");
    const uint32_t autoProfileWarmup=warmupFrames ? uint32_t(std::clamp(std::atoi(warmupFrames->c_str()),0,1200)):0;
    const auto stopFrames=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_STOP_FRAMES");
    const uint32_t autoProfileStopFrames=stopFrames ? uint32_t(std::clamp(std::atoi(stopFrames->c_str()),0,1200)):0;
    const auto profileStep=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_STEP_SECONDS");
    const float autoProfileStep=autoProfilePlay && profileStep ? std::strtof(profileStep->c_str(),nullptr):0.f;
    uint32_t autoProfileStoppedFrames=0;
    bool autoProfileComplete=false;
    struct RetiredSceneFrame
    {
        uint64_t token = 0;
        Engine::Scene::Scene::RetiredGraphicsResources resources;
    };
    std::deque<RetiredSceneFrame> retiredSceneFrames;
    window->OnUpdate = [&]()
    {
        using Clock = std::chrono::steady_clock;
        const auto frameBegin = Clock::now();
        // Start() may run after rendering the previous update. Include the
        // following update's first simulation/recording work and the subsequent
        // sample reporting that work's update interval in the transition.
        if (pendingPlayStart || playStartFollowupUpdates)
            Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStart);
        if (playStartFollowupUpdates) --playStartFollowupUpdates;
        // Restoration and its first cold rendering work are transitions. Also
        // tag the following samples carrying their delayed update interval.
        if (pendingPlayStopRestore || playStopFollowupUpdates)
            Engine::Editor::FrameActivity::Mark(Engine::Editor::FrameActivity::PlayStop);
        if (playStopFollowupUpdates) --playStopFollowupUpdates;
        const double updateIntervalMs = previousUpdateBegin == Clock::time_point{} ? 0. :
            std::chrono::duration<double,std::milli>(frameBegin-previousUpdateBegin).count();
        previousUpdateBegin = frameBegin;
        bool showTimings = false;
        for (const auto& panel : editorState->GetPanels())
            if (auto* performance = dynamic_cast<Engine::Editor::PerformanceView*>(panel.get());
                performance && (performance->IsOpen() || performance->IsRecording()))
            {
                showTimings = true;
                break;
            }
        const bool captureStageWork=showTimings || static_cast<bool>(frameProfiler);
        const auto stageCounters = [captureStageWork]()
        {
            struct Counters
            {
                uint64_t threadTicks=0;
                Engine::Rendering::RenderJobs::Statistics jobs;
            } result;
            if(!captureStageWork) return result;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            {
                const auto ticks=[](FILETIME value) {
                    return (uint64_t(value.dwHighDateTime)<<32) | value.dwLowDateTime;
                };
                result.threadTicks=ticks(kernel)+ticks(user);
            }
            result.jobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
            return result;
        };
        const auto frameCounters=stageCounters();
        scene->SetUpdateProfilingEnabled(showTimings || static_cast<bool>(frameProfiler));
        if (auto* bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter*>(
            &scene->GetPhysics().GetAdapter()))
            bullet->SetProfilingEnabled(showTimings || static_cast<bool>(frameProfiler));
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11EditorRenderer*>(renderer))
            dx11->SetGpuTimingEnabled(showTimings || static_cast<bool>(frameProfiler));
        if (showTimings) renderer->MarkDirty();
        editorFramePacer.Wait(editorState->GetEditorMaxFps());
        const auto afterPacing = Clock::now();
        const auto pacingCounters=stageCounters();
        // Update delta time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;

        // Update background editor work. Transitions themselves invalidate a
        // frame so the final status/overlay is drawn even when continuous work
        // ended during this update.
        const Engine::Editor::PlayState previousPlayState = playState;
        bool pausedFenced = false;
        const bool wasBuilding = gameBuildManager->IsBuilding();
        const bool wasHotReloadBusy = hotReload->IsBusy();
        Engine::Editor::PostBuildAction postBuildAction;
        if (autoProfilePlay && autoProfileTick == 30u)
            gameBuildManager->PlayInEditor();
        gameBuildManager->Update(playState, postBuildAction);
        hotReload->Update(window->IsFocused());
        finishPlayStop();
        if (previousPlayState == Engine::Editor::PlayState::Playing &&
            playState == Engine::Editor::PlayState::Paused)
        {
            scene->FenceBackgroundWork();
            pausedFenced = true;
        }
        if(editorState->PollBackgroundWork()) renderer->MarkDirty();
        if (playState != Engine::Editor::PlayState::Playing)
        {
            scene->SetGameplayInputEnabled(false);
            if (previousPlayState == Engine::Editor::PlayState::Playing)
                window->ReleaseGameplayInput();
        }
        const auto afterBackground = Clock::now();
        const auto backgroundCounters=stageCounters();
        if (previousPlayState != playState ||
            wasBuilding != gameBuildManager->IsBuilding() ||
            wasHotReloadBusy != hotReload->IsBusy())
            renderer->MarkDirty();

        // Let an auto attach request reach VS Code before scene Start() runs.
        const bool debuggerAttached = IsDebuggerPresent();
        if (waitingForPlayDebugger &&
            playState == Engine::Editor::PlayState::Playing &&
            (debuggerAttached || Clock::now() >= debuggerDeadline))
        {
            if (auto* console = editorState->GetConsole())
                console->AddLog(debuggerAttached
                    ? Engine::Editor::ConsoleView::Level::Info
                    : Engine::Editor::ConsoleView::Level::Warning,
                    debuggerAttached
                    ? "[Debug] VS Code debugger attached."
                    : "[Debug] VS Code did not attach; continuing Play.");
            waitingForPlayDebugger = false;
            beginPlayScene();
        }

        const auto pipelineViewsMatch = [&]()
        {
            for (const auto& panel : editorState->GetPanels())
            {
                auto* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                if (!view || !view->IsOpen() || !view->NeedsRender() || !view->IsRenderVisible()) continue;
                Engine::Scene::Scene* bound = nullptr;
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(view)) bound = sceneView->GetScene();
                else if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(view)) bound = gameView->GetScene();
                if (bound != scene) return false;
            }
            return true;
        };
        bool usePlayPipeline = playPipeline && playState == Engine::Editor::PlayState::Playing &&
            !waitingForPlayDebugger && !pendingPlayStart && pipelineViewsMatch();
        scene->ResetGrassPerformance();
        // Tick game objects while playing
        if (playState == Engine::Editor::PlayState::Playing &&
            !waitingForPlayDebugger && !pendingPlayStart &&
            gameBuildManager->GetPlayState() == Engine::Editor::PlayState::Playing)
        {
            if (resetPlayDelta)
            {
                // Keep the transition frame out of simulation time.
                dt = 0.f;
                QueryPerformanceCounter(&lastCounter);
                resetPlayDelta = false;
            }
            else if (autoProfileStep>0.f && autoProfileStep<=.1f && std::isfinite(autoProfileStep))
                dt=autoProfileStep;
            // Physics bounds its own fixed-step work. Keep animation on wall
            // time so a slow render frame does not slow down playback.
            if (!usePlayPipeline)
            {
                scene->Update(std::max(dt, 0.f));
                Engine::Core::SceneManager::ProcessPendingSceneLoad();
            }
        }
        else if (playState == Engine::Editor::PlayState::Stopped)
        {
            // Explicit previews advance in Edit mode without running scripts,
            // physics, or every AnimationManager in the scene.
            bool previewAdvanced = false;
            std::unordered_set<Engine::Scene::Scene*> previewScenes{scene};
            for (const auto& panel : editorState->GetPanels())
                if (auto* view = dynamic_cast<Engine::Editor::SceneView*>(panel.get());
                    view && view->IsOpen() && view->GetScene())
                    previewScenes.insert(view->GetScene());
            for (auto* previewScene : previewScenes)
                previewAdvanced = previewScene->AdvanceEditorPreviews(dt) || previewAdvanced;
            if (previewAdvanced) renderer->MarkDirty();
        }
        const auto afterSceneUpdate = Clock::now();
        const auto updateCounters=stageCounters();

        double prepareMilliseconds = 0.0;
        double renderEntryMilliseconds = 0.0;
        double clearMilliseconds = 0.0;
        double scenePrepareMilliseconds = 0.0;
        double viewMilliseconds = 0.0;
        double shadowSubmitMilliseconds = 0.0;
        double mainSubmitMilliseconds = 0.0;
        double editorUiMilliseconds = 0.0;
        uint32_t renderedViews = 0;
        uint32_t renderedSceneViews = 0;
        uint32_t renderedGameViews = 0;
        uint64_t renderedViewPixels = 0;
        uint32_t gameViewWidth = 0;
        uint32_t gameViewHeight = 0;
        uint32_t submittedShadowDraws = 0;
        Engine::Rendering::RenderCpuTimings renderCpu;
        std::vector<Engine::Scene::Scene*> frameScenes;
        bool pipelineFrameSubmitted = false;

        const auto needsContinuousRendering = [&]()
        {
            return playState == Engine::Editor::PlayState::Playing ||
                gameBuildManager->IsBuilding() || hotReload->IsBusy() ||
                editorState->IsLoadingOverlayVisible() ||
                uiBackend->NeedsContinuousRendering();
        };
        if (needsContinuousRendering())
            renderer->MarkDirty();

        // Update window title
        const std::string title = std::string("Engine Editor - ") +
            editorState->GetActiveDocumentName() +
            (engineDevelopmentMode ? " - Engine Sandbox" : "");
        if (title != displayedWindowTitle)
        {
            displayedWindowTitle = title;
            SetWindowTextA(window->GetHWND(), displayedWindowTitle.c_str());
        }

        struct RecordedView
        {
            Engine::Editor::View* view;
            std::shared_ptr<Engine::Graphics::GraphicsCommandPacket> shadow, color;
            uint32_t shadowDraws;
        };
        std::vector<RecordedView> recordedViews;
        std::vector<Engine::Editor::EditorPlayPipeline::Commands> frameCommands;
        std::shared_ptr<Engine::Graphics::GraphicsCommandPacket> computeCommands;
        double pipelineRecordMs = 0;
        double pipelineComputeRecordMs=0,pipelineSnapshotMs=0,pipelineViewRecordMs=0;
        Engine::Editor::EditorPlayPipeline::Timings pipelineTimings;
        Engine::Rendering::GrassPerformance recordedGrass;
        bool uiDrawn = false;
        double uiCpuMilliseconds = 0;
        const auto drawEditorUi = [&]()
        {
            if (uiDrawn) return;
            editorState->GetToolbarTimings() = {};
            const auto beforeUi = Clock::now();
            const auto beforeUiCounters = stageCounters();
            uiBackend->DrawEditor(*editorState, playState, gameBuildManager.get());
            editorUiMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - beforeUi).count();
            const auto afterUiCounters = stageCounters();
            if (afterUiCounters.threadTicks >= beforeUiCounters.threadTicks)
                uiCpuMilliseconds = double(afterUiCounters.threadTicks - beforeUiCounters.threadTicks) / 10000.;
            uiDrawn = true;
            for (auto& panel : editorState->GetPanels())
                if (auto* view = dynamic_cast<Engine::Editor::View*>(panel.get()))
                {
                    const bool visibilityChanged = view->ConsumeVisibilityChanged();
                    const bool renderSizeChanged = view->ConsumeRenderSizeChanged();
                    if (visibilityChanged || renderSizeChanged) renderer->MarkDirty();
                }
        };
        // Render frame
        // A viewport resize can replace the texture referenced by ImGui. Apply
        // last frame's size before building UI draw data, never afterwards.
        if (usePlayPipeline && sizeDx11ViewsToPanels)
            for (const auto& panel : editorState->GetPanels())
                if (auto* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                    view && view->IsOpen() && view->IsRenderVisible())
                    view->ApplyRequestedRenderSize(renderer->GetNativeDeviceHandle());
        const auto submissionBefore = renderer->GetSubmittedFrameCount();
        const uint64_t submissionTokenBefore = renderer->GetSubmittedFrameToken();
        submittingEditorFrame = true;
        renderer->RenderIfNeeded([&]()
        {
            const auto drawBegin = Clock::now();
            renderEntryMilliseconds = std::chrono::duration<double, std::milli>(
                drawBegin - afterSceneUpdate).count();
            // Finish live UI edits before recording immutable replay commands.
            if (usePlayPipeline)
            {
                drawEditorUi();
                if (gameBuildManager->GetPlayState() != Engine::Editor::PlayState::Playing ||
                    pendingPlayStopRestore) usePlayPipeline = false;
                if (!pipelineViewsMatch()) usePlayPipeline = false;
            }
            if (usePlayPipeline) scene->SetDeferredGraphicsRetirement(true);
            // Prepare render data for every scene that will render this frame.
            // Prefab edit opens its own scene-backed viewport, so relying on
            // the main scene alone can leave prefab views with stale/missing
            // draw state.
            const auto scenePrepareStart = Clock::now();
            std::unordered_set<Engine::Scene::Scene*> preparedScenes;
            for (auto& panel : editorState->GetPanels())
            {
                if (!panel || !panel->NeedsRender() || !panel->IsOpen())
                    continue;
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(panel.get()))
                {
                    if (!sceneView->IsRenderVisible()) continue;
                    if (Engine::Scene::Scene* panelScene = sceneView->GetScene())
                    {
                        panelScene->SetBeforeGpuResourceDestruction(
                            [renderer] { renderer->WaitIdle(); });
                        panelScene->SetDeferredGraphicsRetirement(true);
                        if (preparedScenes.insert(panelScene).second)
                            panelScene->PrepareRenderFrame(!usePlayPipeline);
                    }
                    continue;
                }
                if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(panel.get()))
                {
                    if (!gameView->IsRenderVisible()) continue;
                    if (Engine::Scene::Scene* panelScene = gameView->GetScene())
                    {
                        panelScene->SetBeforeGpuResourceDestruction(
                            [renderer] { renderer->WaitIdle(); });
                        panelScene->SetDeferredGraphicsRetirement(true);
                        if (preparedScenes.insert(panelScene).second)
                            panelScene->PrepareRenderFrame(!usePlayPipeline);
                    }
                }
            }
            frameScenes.assign(preparedScenes.begin(), preparedScenes.end());
            const auto afterPrepare = Clock::now();
            if (usePlayPipeline)
            {
                pipelineFrameSubmitted = true;
                const auto recordStart = Clock::now();
                computeCommands = playPipeline->AcquireCommands();
                scene->RecordGpuParticleSimulation(computeCommands.get());
                frameCommands.push_back(computeCommands);
                const auto computeRecordEnd=Clock::now();
                pipelineComputeRecordMs=std::chrono::duration<double,std::milli>(computeRecordEnd-recordStart).count();
                if (preparedScenes.count(scene))
                {
                    auto drawPacket = scene->TakeNativeRenderPacket();
                    scene->RetainNativePacketMeshBuffers(*drawPacket, *computeCommands);
                    pipelineSnapshotMs=std::chrono::duration<double,std::milli>(Clock::now()-computeRecordEnd).count();
                    for (const auto& panel : editorState->GetPanels())
                    {
                        auto* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                        if (!view || !view->IsOpen() || !view->NeedsRender() || !view->IsRenderVisible()) continue;
                        RecordedView recorded{view,
                            playPipeline->AcquireCommands(),
                            playPipeline->AcquireCommands(), 0};
                        const auto viewRecordStart=Clock::now();
                        if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(view))
                            sceneView->RecordRenderCommands(*recorded.shadow, *recorded.color, drawPacket.get());
                        else if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(view))
                            gameView->RecordRenderCommands(*recorded.shadow, *recorded.color, drawPacket.get());
                        else continue;
                        pipelineViewRecordMs+=std::chrono::duration<double,std::milli>(Clock::now()-viewRecordStart).count();
                        recorded.shadowDraws = scene->GetShadowDebugSnapshot().submittedCasterDrawCount;
                        renderCpu += scene->GetLastRenderCpuTimings();
                        frameCommands.push_back(recorded.shadow);
                        frameCommands.push_back(recorded.color);
                        recordedViews.push_back(std::move(recorded));
                    }
                }
                pipelineRecordMs = std::chrono::duration<double, std::milli>(Clock::now() - recordStart).count();
                auto input = Engine::Core::CaptureNativeInput(window->GetHWND());
                // Embedded UI coordinates override window coordinates. If no
                // Game surface has supplied input, suppress native-window UI.
                input.clientWidth = input.clientHeight = 0;
                scene->CaptureEmbeddedUiInput(input);
                recordedGrass = scene->GetGrassPerformance();
                playPipeline->Begin(*scene, std::max(dt, 0.f), std::move(input));
            }
            prepareMilliseconds = std::chrono::duration<double, std::milli>(
                afterPrepare - afterSceneUpdate).count();
            scenePrepareMilliseconds = std::chrono::duration<double, std::milli>(
                afterPrepare - scenePrepareStart).count();

            using FramePlan = Engine::Rendering::RenderFramePlan;
            constexpr FramePlan::ResourceId EditorSceneData = 1;
            constexpr FramePlan::ResourceId EditorParticleState = 2;
            constexpr FramePlan::ResourceId MainWindowColor = 3;
            constexpr FramePlan::ResourceId FirstViewResource = 16;
            FramePlan framePlan;
            framePlan.AddPass("Clear editor window",
                {{MainWindowColor, FramePlan::Access::Write}}, [&]()
            {
                const auto started = Clock::now();
                renderer->Clear(projectSettings.clearColor.r, projectSettings.clearColor.g,
                    projectSettings.clearColor.b);
                clearMilliseconds = std::chrono::duration<double, std::milli>(
                    Clock::now() - started).count();
            });
            framePlan.AddPass("Particle simulation",
                {{EditorSceneData, FramePlan::Access::Read},
                 {EditorParticleState, FramePlan::Access::ReadWrite}}, [&]()
            {
                if (auto* provider = renderer->GetGraphicsProvider())
                    if (auto* factory = provider->GetContextFactory())
                    {
                        factory->SetCommandBuffer(renderer->GetCurrentCommandBuffer());
                        auto computeContext = factory->CreateContext();
                        if (usePlayPipeline)
                        {
                            if (computeContext) computeCommands->Replay(*computeContext);
                        }
                        else for (Engine::Scene::Scene* prepared : preparedScenes)
                            prepared->RecordGpuParticleSimulation(computeContext.get());
                    }
            });

            std::vector<FramePlan::ResourceUse> viewUses{
                {EditorSceneData, FramePlan::Access::Read},
                {EditorParticleState, FramePlan::Access::Read},
                {MainWindowColor, FramePlan::Access::ReadWrite}};
            FramePlan::ResourceId nextViewResource = FirstViewResource;
            for (auto& panel : editorState->GetPanels())
                if (panel && panel->NeedsRender() && panel->IsOpen())
                    if (auto* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                        view && view->IsRenderVisible())
                    {
                        viewUses.push_back({nextViewResource++, FramePlan::Access::Write});
                        viewUses.push_back({nextViewResource++, FramePlan::Access::Write});
                    }
            framePlan.AddPass("Editor scene and game views", std::move(viewUses), [&]()
            {
                for (auto& panel : editorState->GetPanels())
                {
                    if (!panel) continue;
                    if (!panel->NeedsRender() || !panel->IsOpen()) continue;
                    Engine::Editor::View* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                    if (!view || !view->IsRenderVisible()) continue;
                    if (sizeDx11ViewsToPanels && !uiDrawn)
                        view->ApplyRequestedRenderSize(renderer->GetNativeDeviceHandle());
                    renderedViewPixels += static_cast<uint64_t>(view->GetWidth()) *
                        view->GetHeight();
                
                void* cmdList = renderer->GetCurrentCommandBuffer();
                void* rtvHandle = renderer->GetCurrentRenderTargetHandle();
                
                Engine::Scene::Scene* renderedScene=nullptr;
                if(auto* scenePanel=dynamic_cast<Engine::Editor::SceneView*>(view)) renderedScene=scenePanel->GetScene();
                else if(auto* gamePanel=dynamic_cast<Engine::Editor::GameView*>(view)) renderedScene=gamePanel->GetScene();
                const RecordedView* recorded = nullptr;
                if (usePlayPipeline)
                    for (const auto& candidate : recordedViews)
                        if (candidate.view == view) { recorded = &candidate; break; }
                const auto replay = [&](const Engine::Graphics::GraphicsCommandPacket& commands, void* cmd)
                {
                    auto* factory = renderer->GetGraphicsProvider()->GetContextFactory();
                    factory->SetCommandBuffer(cmd);
                    auto context = factory->CreateContext();
                    if (context) commands.Replay(*context);
                };
                const auto viewBegin = Clock::now();
                    view->Render(cmdList, rtvHandle,
                    [&, view](void* cmd)
                    {
                        const auto begin = Clock::now();
                        if (recorded) replay(*recorded->color, cmd);
                        else
                        {
                            if(renderedScene) renderedScene->ResetRenderCpuTimings();
                            view->Render3D(cmd);
                            if(renderedScene) renderCpu+=renderedScene->GetLastRenderCpuTimings();
                        }
                        mainSubmitMilliseconds += std::chrono::duration<double,
                            std::milli>(Clock::now() - begin).count();
                    },
                    [&, view](void* cmd)
                    {
                        const auto begin = Clock::now();
                        if (recorded) replay(*recorded->shadow, cmd);
                        else view->RenderShadow3D(cmd);
                        shadowSubmitMilliseconds += std::chrono::duration<double,
                            std::milli>(Clock::now() - begin).count();
                    });
                viewMilliseconds += std::chrono::duration<double,
                    std::milli>(Clock::now() - viewBegin).count();
                ++renderedViews;
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(view))
                {
                    ++renderedSceneViews;
                    if (recorded) submittedShadowDraws += recorded->shadowDraws;
                    else if (auto* panelScene = sceneView->GetScene())
                        submittedShadowDraws += panelScene->GetShadowDebugSnapshot()
                            .submittedCasterDrawCount;
                }
                else if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(view))
                {
                    ++renderedGameViews;
                    gameViewWidth = gameView->GetWidth();
                    gameViewHeight = gameView->GetHeight();
                    if (recorded) submittedShadowDraws += recorded->shadowDraws;
                    else if (auto* panelScene = gameView->GetScene())
                        submittedShadowDraws += panelScene->GetShadowDebugSnapshot()
                            .submittedCasterDrawCount;
                }
                }
            });

            // No live scene reads are allowed once simulation starts.
            framePlan.AddPass("Build editor UI", {}, [&]()
            {
                if (!uiDrawn) drawEditorUi();
            });
            framePlan.AddRendererPass("Editor UI overlay",
                {{MainWindowColor, FramePlan::Access::ReadWrite}});
            framePlan.Execute();
        });
        submittingEditorFrame = false;
        editorState->ReleaseRetiredAnimationDocuments();
        // A minimized/out-of-date surface can skip the draw callback entirely.
        // Keep the simulation ticking without requiring a submitted view.
        if (usePlayPipeline && !uiDrawn && !playPipeline->Busy() &&
            gameBuildManager->GetPlayState() == Engine::Editor::PlayState::Playing)
        {
            auto input = Engine::Core::CaptureNativeInput(window->GetHWND());
            input.clientWidth = input.clientHeight = 0;
            scene->CaptureEmbeddedUiInput(input);
            playPipeline->Begin(*scene, std::max(dt, 0.f), std::move(input));
        }
        if (playPipeline && playPipeline->Busy())
        {
            pipelineTimings = playPipeline->Finish(std::move(frameCommands));
            scene->AddGrassPerformance(recordedGrass);
            uint32_t occlusionCulled = 0;
            for (const auto& recorded : recordedViews)
                occlusionCulled += recorded.color->GetReplayOcclusionCulledCount();
            scene->SetLastNativeOcclusionCulledCount(occlusionCulled);
            Engine::Core::SceneManager::ProcessPendingSceneLoad();
        }
        if (deferredClearGameplayFocus)
        {
            deferredClearGameplayFocus = false;
            scene->SetGameplayInputEnabled(false);
            uiBackend->ClearFocus();
            renderer->MarkDirty();
        }
        if (deferredAssetCheck)
        {
            deferredAssetCheck = false;
            editorState->CheckAssetLocations();
            renderer->MarkDirty();
        }
        if (deferredResize)
        {
            const auto [width, height] = *deferredResize;
            deferredResize.reset();
            if (playPipeline) playPipeline->Drain();
            resizeEditor(width, height);
        }
        const auto afterRender = Clock::now();
        const auto submissionAfter = renderer->GetSubmittedFrameCount();
        const auto collectRetiredSceneFrames = [&]()
        {
            while (!retiredSceneFrames.empty() &&
                renderer->IsFrameComplete(retiredSceneFrames.front().token))
                retiredSceneFrames.pop_front();
        };
        collectRetiredSceneFrames();
        const bool submittedEditorFrame = renderer->GetSubmittedFrameToken() >
            submissionTokenBefore;
        if (submittedEditorFrame && !pipelineFrameSubmitted)
        {
            const uint64_t token = renderer->GetSubmittedFrameToken();
            for (auto* frameScene : frameScenes)
            {
                if (!frameScene) continue;
                auto resources = frameScene->TakeRetiredGraphicsResources();
                if (!resources.Empty())
                    retiredSceneFrames.push_back({token, std::move(resources)});
            }
        }
        while (retiredSceneFrames.size() >= 3u)
        {
            renderer->WaitForFrame(retiredSceneFrames.front().token);
            collectRetiredSceneFrames();
            if (!retiredSceneFrames.empty() &&
                !renderer->IsFrameComplete(retiredSceneFrames.front().token))
                break;
        }
        const auto renderCounters=stageCounters();
        if (!pausedFenced &&
            previousPlayState == Engine::Editor::PlayState::Playing &&
            playState == Engine::Editor::PlayState::Paused)
            scene->FenceBackgroundWork();
        finishPlayStop();
        // UI callbacks and the simulation tick have returned before a script
        // module can replace their component code.
        if (hotReload->PublishPending()) renderer->MarkDirty();
        if (pendingPlayStart && playState == Engine::Editor::PlayState::Playing &&
            !waitingForPlayDebugger)
        {
            beginPlayScene();
        }

        // Scene requests made while drawing the UI now have a presented frame
        // with the loading overlay before deserialization blocks this thread.
        editorState->ProcessPendingSceneLoad();
        const auto afterPendingLoad = Clock::now();
        const auto loadCounters=stageCounters();
        {
            Engine::Editor::EditorFrameProfileSample sample{};
            const auto fillStage=[&](size_t index, auto begin, auto end,
                const auto& first, const auto& last)
            {
                auto& stage=sample.stages[index];
                stage.wallMs=std::chrono::duration<double,std::milli>(end-begin).count();
                stage.mainBusyMs=last.threadTicks>=first.threadTicks
                    ? double(last.threadTicks-first.threadTicks)/10000. : 0.;
                stage.workerMs=double(last.jobs.workerNs-first.jobs.workerNs)/1.e6;
                stage.queueMs=double(last.jobs.queueNs-first.jobs.queueNs)/1.e6;
                stage.dependencyMs=double(last.jobs.dependencyNs-first.jobs.dependencyNs)/1.e6;
                stage.waitMs=double(last.jobs.waitNs-first.jobs.waitNs)/1.e6;
                if(index==3)
                    stage.renderStallMs=std::max(0.,stage.wallMs-stage.mainBusyMs);
            };
            fillStage(0,frameBegin,afterPacing,frameCounters,pacingCounters);
            fillStage(1,afterPacing,afterBackground,pacingCounters,backgroundCounters);
            fillStage(2,afterBackground,afterSceneUpdate,backgroundCounters,updateCounters);
            fillStage(3,afterSceneUpdate,afterRender,updateCounters,renderCounters);
            fillStage(4,afterRender,afterPendingLoad,renderCounters,loadCounters);
            sample.updateIntervalMs = updateIntervalMs;
            sample.idleWaitMs = previousIdleWaitMs;
            sample.submissionCountValid = submissionBefore && submissionAfter &&
                previousSubmissionSample != Clock::time_point{} && *submissionAfter >= *submissionBefore;
            sample.submittedFrames = sample.submissionCountValid ? *submissionAfter-*submissionBefore : 0;
            sample.submissionIntervalMs = previousSubmissionSample == Clock::time_point{} ? 0. :
                std::chrono::duration<double,std::milli>(afterRender-previousSubmissionSample).count();
            previousSubmissionSample = afterRender;
            sample.frameBegin = frameBegin;
            sample.afterPacing = afterPacing;
            sample.afterBackground = afterBackground;
            sample.afterSceneUpdate = afterSceneUpdate;
            sample.afterRender = afterRender;
            sample.afterPendingLoad = afterPendingLoad;
            sample.scenePath = editorState->GetCurrentScenePath();
            sample.playPipeline = usePlayPipeline;
            sample.uiCpuMilliseconds = uiCpuMilliseconds;
            if (uiDrawn)
            {
                const auto& timings = editorState->GetToolbarTimings();
                sample.uiToolbarTimingValid = true;
                sample.uiToolbarMilliseconds = timings.totalMilliseconds;
                sample.uiToolbarLodSummaryMilliseconds = timings.lodSummaryMilliseconds;
                sample.uiToolbarSectionMilliseconds = timings.sectionMilliseconds;
                sample.uiToolbarSectionDraws = timings.sectionDraws;
                sample.uiToolbarLodSummaryCalls = timings.lodSummaryCalls;
            }
            for (const auto& panel : editorState->GetPanels())
                if (panel)
                    sample.uiPanels.push_back({panel->GetTitle(), panel->GetLastUiDrawMilliseconds()});
            sample.workerPending = loadCounters.jobs.pending;
            sample.workerCount = loadCounters.jobs.workers;
            sample.startup = recordedFrameCount++ < 2;
            sample.activity = Engine::Editor::FrameActivity::Consume();
            sample.writer = frameProfiler.GetWriterStatistics();
            sample.simulationMilliseconds = pipelineTimings.updateMs;
            sample.simulationCpuMilliseconds = pipelineTimings.updateCpuMs;
            sample.simulationWaitMilliseconds = pipelineTimings.waitMs;
            sample.simulationOverlapMilliseconds = pipelineTimings.overlapMs;
            sample.simulationGpuWaitMilliseconds = pipelineTimings.gpuWaitMs;
            sample.pipelineRecordMilliseconds = pipelineRecordMs;
            sample.pipelineComputeRecordMilliseconds=pipelineComputeRecordMs;
            sample.pipelineSnapshotMilliseconds=pipelineSnapshotMs;
            sample.pipelineViewRecordMilliseconds=pipelineViewRecordMs;
            sample.pipelineQueueWaitMilliseconds = pipelineTimings.queueWaitMs;
            sample.pipelineQueueWaits = pipelineTimings.queueWaits;
            sample.pipelineRetainedFrames = pipelineTimings.retainedFrames;
            sample.prepareMilliseconds = prepareMilliseconds;
            sample.renderEntryMilliseconds = renderEntryMilliseconds;
            sample.clearMilliseconds = clearMilliseconds;
            sample.scenePrepareMilliseconds = scenePrepareMilliseconds;
            sample.viewMilliseconds = viewMilliseconds;
            sample.shadowSubmitMilliseconds = shadowSubmitMilliseconds;
            sample.mainSubmitMilliseconds = mainSubmitMilliseconds;
            sample.renderCpu=renderCpu;
            sample.editorUiMilliseconds = editorUiMilliseconds;
            sample.renderedViews = renderedViews;
            sample.renderedSceneViews = renderedSceneViews;
            sample.renderedGameViews = renderedGameViews;
            sample.renderedViewPixels = renderedViewPixels;
            sample.gameViewWidth = gameViewWidth;
            sample.gameViewHeight = gameViewHeight;
            sample.submittedShadowDraws = submittedShadowDraws;
            sample.deltaSeconds = dt;
            sample.playSnapshotBytes=editorState->GetPlaySnapshotMemoryBytes();
            sample.sceneHistoryBytes=editorState->GetSceneHistoryMemoryBytes();
            sample.playing = playState == Engine::Editor::PlayState::Playing;
            sample.measured=!autoProfilePlay || (sample.playing && renderedGameViews>0 &&
                autoProfilePlayingFrames>=autoProfileWarmup && !autoProfileComplete);
            if (frameProfiler)
                frameProfiler.Record(sample, scene, renderer);
            for (const auto& panel : editorState->GetPanels())
                if (auto* performance = dynamic_cast<Engine::Editor::PerformanceView*>(panel.get());
                    performance && (performance->IsOpen() || performance->IsRecording()))
                    performance->SetFrameTimings(sample, renderer);
        }

        // Avoid spinning the editor loop while idle. Window input wakes this
        // wait immediately; the short timeout also services asynchronous
        // directory notifications and other background state promptly.
        previousIdleWaitMs = 0;
        if (!renderer->IsDirty() && !needsContinuousRendering())
        {
            const auto idleBegin = Clock::now();
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            previousIdleWaitMs = std::chrono::duration<double,std::milli>(Clock::now()-idleBegin).count();
        }
        if(autoProfilePlay)
        {
            if (autoProfileComplete && autoProfileStopFrames>0 &&
                autoProfileStoppedFrames<autoProfileStopFrames)
            {
                renderer->MarkDirty();
                // Count restored Edit frames only, after Stop's deferred restore.
                if (playState==Engine::Editor::PlayState::Stopped && !pendingPlayStopRestore &&
                    ++autoProfileStoppedFrames>=autoProfileStopFrames)
                {
                    frameProfiler.Stop();
                    if(!autoProfileKeepOpen) PostMessageW(window->GetHWND(),WM_CLOSE,0,0);
                }
            }
            ++autoProfileTick;
            if(!autoProfileComplete && playState==Engine::Editor::PlayState::Playing && !waitingForPlayDebugger && renderedGameViews>0 &&
                ++autoProfilePlayingFrames>=autoProfileWarmup+autoProfilePlayFrames)
            {
                autoProfileComplete=true;
                if (autoProfileStopFrames>0) gameBuildManager->Stop();
                else
                {
                    frameProfiler.Stop();
                    if(!autoProfileKeepOpen) { gameBuildManager->Stop(); PostMessageW(window->GetHWND(),WM_CLOSE,0,0); }
                }
            }
        }
    };
    OutputDebugStringA("[Main] OnUpdate callback set\n");

    // Draw the first frame before exposing the native window. The initial
    // editor layout can take time, and an unpainted window appears blank.
    splash.SetProgress(0.95f, L"Drawing editor...");
    renderer->MarkDirty();
    window->OnUpdate();
    OutputDebugStringA("[Main] Showing window...\n");
    splash.HoldAboveMainWindow();
    window->Show();
    // A hidden swap chain can report occlusion and discard its first Present.
    // Present once more after showing before removing the splash.
    renderer->MarkDirty();
    window->OnUpdate();
    splash.Close();
    editorState->RequestCodeEditorAttach(true);
    OutputDebugStringA("[Main] Window shown, entering message loop...\n");

    int result = window->Run();

    editorState->SaveAnimationEditorSession();

    // Panels release UI texture registrations while the selected package is
    // still alive. The renderer itself is owned by EditorState.
    gameBuildManager->Stop();
    finishPlayStop();
    gameBuildManager.reset();
    hotReload.reset();
    Engine::Core::SceneManager::CancelPendingSceneLoad();
    Engine::Core::SceneManager::SetOnSceneLoaded({});
    scene->FenceBackgroundWork();
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    scene->SetBeforeGpuResourceDestruction({});
    playPipeline.reset();
    renderer->WaitIdle();
    retiredSceneFrames.clear();
    editorState->ReleaseUiPanels();
    // ImGui's Vulkan backend destroys device resources. It must shut down
    // after view registrations, while EditorState still owns a live device.
    uiBackend->Shutdown();
    renderer->SetUiRenderHooks({});
    renderer->SetUiTextureHooks({});
    window->OnUpdate = {};
    window->OnResize = {};
    window->WndProcHook = {};
    window->OnEscapePressed = {};
    window->OnApplicationFocusLost = {};
    window->OnApplicationFocusGained = {};
    window->OnInputBegin = {};
    window->OnInputEnd = {};
    editorState.reset();
    uiBackend.reset();

    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}
