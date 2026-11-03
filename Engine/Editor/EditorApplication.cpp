#include "Engine/Editor/EditorApplication.h"
#include "Engine/Editor/Runtime/EditorPlayPipeline.h"
#include "Engine/Editor/Startup/EditorCommandLine.h"
#include "Engine/Editor/Startup/EditorStartupSettings.h"
#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"
#include "Core/Window.h"
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
#include "Core/View/View.h"
#include "Engine/Editor/EditorState.h"
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
#define PROJECT_SCRIPTS_PATH "build/Debug/Debug/ProjectScripts.dll"
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

    if (EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_SCENE"))
    {
        if (const auto width = EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_WIDTH"))
            projectSettings.viewportWidth = static_cast<uint32_t>(std::clamp(
                std::atoi(width->c_str()), 320, 4096));
        if (const auto height = EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_HEIGHT"))
            projectSettings.viewportHeight = static_cast<uint32_t>(std::clamp(
                std::atoi(height->c_str()), 240, 2160));
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
    if (EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_GAME_VIEW").has_value())
        for (const auto& panel : editorState->GetPanels())
        {
            if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(panel.get()))
                sceneView->SetOpen(false);
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
        OutputDebugStringA(("[Main] Loading default scene: " + startupScene + "\n").c_str());
        editorState->LoadStartupScene(startupScene);
    }
    // Character animation documents open explicitly. A saved session must not
    // reopen a character tab after the user closes the Animation Editor.
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
    bool resetPlayDelta = false;
    const auto finishPlayStop = [&]()
    {
        if (!pendingPlayStopRestore) return;
        pendingPlayStopRestore = false;
        if (playPipeline) playPipeline->Drain();
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        scene->FenceBackgroundWork();
        Engine::Core::SceneManager::SetOnSceneLoaded({});
        editorState->RestorePlayModeScene();
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
        pendingPlayStart = false;
        if (!editorState->CapturePlayModeScene())
        {
            editorState->SetLoadingOverlay(false);
            gameBuildManager->Stop();
            return;
        }
        scene = editorState->GetScene();
        scene->Start();
        resetPlayDelta = true;
        editorState->SetLoadingOverlay(false);
    };
    gameBuildManager->OnPlayStart = [&]()
    {
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        scene->FenceBackgroundWork();
        scene->SetGameplayInputEnabled(false);
        const bool requested = editorState->RequestCodeEditorAttach();
        pendingPlayStart = true;
        editorState->SetLoadingOverlay(true, "Preparing runtime scene...");
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
        scene->SetGameplayInputEnabled(false);
        window->ReleaseGameplayInput();
        waitingForPlayDebugger = false;
        pendingPlayStart = false;
        editorState->SetLoadingOverlay(false);
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
    const bool autoProfileKeepOpen=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_KEEP_OPEN").has_value();
    const auto warmupFrames=EnvironmentVariable("ENGINE_EDITOR_AUTO_PROFILE_WARMUP_FRAMES");
    const uint32_t autoProfileWarmup=warmupFrames ? uint32_t(std::clamp(std::atoi(warmupFrames->c_str()),0,1200)):0;
    bool autoProfileComplete=false;
    window->OnUpdate = [&]()
    {
        using Clock = std::chrono::steady_clock;
        const auto frameBegin = Clock::now();
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
                // Runtime cloning is loading time, not simulation time.
                dt = 0.f;
                QueryPerformanceCounter(&lastCounter);
                resetPlayDelta = false;
            }
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
            for (Engine::Scene::Scene* previewScene : previewScenes)
            {
                for(auto* emitter:previewScene->GetComponents<Engine::Components::ParticleEmitter>())
                    if(emitter->Owner && emitter->Owner->IsEnabledInHierarchy() && emitter->IsEditorPreviewRunning())
                    {
                        emitter->Advance(std::max(dt,0.f),emitter->Owner->transform.GetWorldMatrix());
                        previewAdvanced=true;
                    }
                for(auto* animation:previewScene->GetComponents<Engine::Components::AnimationManager>())
                    if(animation->Owner && animation->Owner->IsEnabledInHierarchy() && animation->IsEditorPreviewRunning())
                    { animation->Tick(std::max(dt,0.f)); previewAdvanced=true; }
            }
            for (auto* previewScene : previewScenes)
            {
                previewAdvanced=previewScene->UpdateSpatialScripts(true) || previewAdvanced;
                previewScene->CommitPendingObjectChanges();
            }
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
        Engine::Editor::EditorPlayPipeline::Timings pipelineTimings;
        Engine::Rendering::GrassPerformance recordedGrass;
        bool uiDrawn = false;
        const auto drawEditorUi = [&]()
        {
            if (uiDrawn) return;
            const auto beforeUi = Clock::now();
            uiBackend->DrawEditor(*editorState, playState, gameBuildManager.get());
            editorUiMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - beforeUi).count();
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
            renderer->Clear(projectSettings.clearColor.r, projectSettings.clearColor.g, projectSettings.clearColor.b);
            const auto afterClear = Clock::now();
            clearMilliseconds = std::chrono::duration<double, std::milli>(
                afterClear - drawBegin).count();

            // Prepare render data for every scene that will render this frame.
            // Prefab edit opens its own scene-backed viewport, so relying on
            // the main scene alone can leave prefab views with stale/missing
            // draw state.
            std::unordered_set<Engine::Scene::Scene*> preparedScenes;
            for (auto& panel : editorState->GetPanels())
            {
                if (!panel || !panel->NeedsRender() || !panel->IsOpen())
                    continue;
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(panel.get()))
                {
                    if (!sceneView->IsRenderVisible()) continue;
                    if (Engine::Scene::Scene* panelScene = sceneView->GetScene())
                        if (preparedScenes.insert(panelScene).second)
                            panelScene->PrepareRenderFrame(!usePlayPipeline);
                    continue;
                }
                if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(panel.get()))
                {
                    if (!gameView->IsRenderVisible()) continue;
                    if (Engine::Scene::Scene* panelScene = gameView->GetScene())
                        if (preparedScenes.insert(panelScene).second)
                            panelScene->PrepareRenderFrame(!usePlayPipeline);
                }
            }
            const auto afterPrepare = Clock::now();
            if (usePlayPipeline)
            {
                const auto recordStart = Clock::now();
                computeCommands = playPipeline->AcquireCommands();
                scene->RecordGpuParticleSimulation(computeCommands.get());
                frameCommands.push_back(computeCommands);
                if (preparedScenes.count(scene))
                {
                    auto drawPacket = scene->TakeNativeRenderPacket();
                    scene->RetainNativePacketMeshBuffers(*drawPacket, *computeCommands);
                    for (const auto& panel : editorState->GetPanels())
                    {
                        auto* view = dynamic_cast<Engine::Editor::View*>(panel.get());
                        if (!view || !view->IsOpen() || !view->NeedsRender() || !view->IsRenderVisible()) continue;
                        RecordedView recorded{view,
                            playPipeline->AcquireCommands(),
                            playPipeline->AcquireCommands(), 0};
                        if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(view))
                            sceneView->RecordRenderCommands(*recorded.shadow, *recorded.color, drawPacket.get());
                        else if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(view))
                            gameView->RecordRenderCommands(*recorded.shadow, *recorded.color, drawPacket.get());
                        else continue;
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
            prepareMilliseconds = std::chrono::duration<double, std::milli>(
                afterPrepare - afterSceneUpdate).count();
            scenePrepareMilliseconds = std::chrono::duration<double, std::milli>(
                afterPrepare - afterClear).count();

            // Render 3D panels
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

            // No live scene reads are allowed once simulation starts.
            if (!uiDrawn) drawEditorUi();
        });
        submittingEditorFrame = false;
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
        const auto renderCounters=stageCounters();
        if (!pausedFenced &&
            previousPlayState == Engine::Editor::PlayState::Playing &&
            playState == Engine::Editor::PlayState::Paused)
            scene->FenceBackgroundWork();
        finishPlayStop();
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
            sample.frameBegin = frameBegin;
            sample.afterPacing = afterPacing;
            sample.afterBackground = afterBackground;
            sample.afterSceneUpdate = afterSceneUpdate;
            sample.afterRender = afterRender;
            sample.afterPendingLoad = afterPendingLoad;
            sample.playPipeline = usePlayPipeline;
            sample.simulationMilliseconds = pipelineTimings.updateMs;
            sample.simulationCpuMilliseconds = pipelineTimings.updateCpuMs;
            sample.simulationWaitMilliseconds = pipelineTimings.waitMs;
            sample.simulationOverlapMilliseconds = pipelineTimings.overlapMs;
            sample.simulationGpuWaitMilliseconds = pipelineTimings.gpuWaitMs;
            sample.pipelineRecordMilliseconds = pipelineRecordMs;
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
        if (!renderer->IsDirty() && !needsContinuousRendering())
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
        if(autoProfilePlay)
        {
            ++autoProfileTick;
            if(!autoProfileComplete && playState==Engine::Editor::PlayState::Playing && !waitingForPlayDebugger && renderedGameViews>0 &&
                ++autoProfilePlayingFrames>=autoProfileWarmup+autoProfilePlayFrames)
            {
                autoProfileComplete=true; frameProfiler.Stop();
                if(!autoProfileKeepOpen) { gameBuildManager->Stop(); PostMessageW(window->GetHWND(),WM_CLOSE,0,0); }
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
