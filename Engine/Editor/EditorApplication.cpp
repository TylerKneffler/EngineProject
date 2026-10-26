#include "Engine/Editor/EditorApplication.h"
#include "Engine/Editor/Startup/EditorCommandLine.h"
#include "Engine/Editor/Startup/EditorStartupSettings.h"
#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"
#include "Core/Window.h"
#include "Core/Startup/StartupSplash.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Timing/FramePacer.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/View/View.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/HotReload/EditorHotReload.h"
#include "Engine/Editor/ProjectLauncher.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Engine/Editor/Core/View/Views/GameView.h"
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
    if (!window || !renderer || !scene)
    {
        WriteStartupLog("Editor did not create all required core components");
        return 1;
    }
    Engine::Core::SceneManager::SetActiveScene(scene);
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
    splash.SetProgress(0.75f, L"Loading startup scene...");

    // The opt-in playback audit loads the same bundled scene as the MMD
    // harness, but runs it through the complete editor and UI frame loop.
    const bool autoProfileMmdPlay =
        std::getenv("ENGINE_EDITOR_AUTO_PROFILE_MMD") != nullptr;
    const char* requestedAutoProfileFrames =
        std::getenv("ENGINE_EDITOR_AUTO_PROFILE_FRAMES");
    const uint32_t autoProfilePlayFrames = requestedAutoProfileFrames
        ? static_cast<uint32_t>(std::clamp(
            std::atoi(requestedAutoProfileFrames), 30, 1200))
        : 180u;
    const std::string startupScene = autoProfileMmdPlay
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
    hotReload->BeforeApply = [&]()
    {
        gameBuildManager->Stop();
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
    gameBuildManager->OnPlayStart = [&]()
    {
        const bool requested = editorState->RequestCodeEditorAttach();
        editorState->CapturePlayModeScene();
        waitingForPlayDebugger = requested && !IsDebuggerPresent();
        if (waitingForPlayDebugger)
        {
            debuggerDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(4);
            if (auto* console = editorState->GetConsole())
                console->AddLog(Engine::Editor::ConsoleView::Level::Info,
                    "[Debug] Waiting up to 4 seconds for VS Code to attach.");
        }
        else
            scene->Start();
    };
    gameBuildManager->OnPlayStop = [&]()
    {
        waitingForPlayDebugger = false;
        Engine::Core::SceneManager::CancelPendingSceneLoad();
        editorState->RestorePlayModeScene();
    };
    gameBuildManager->OnBuildComplete = [&](bool)
    {
        editorState->CheckAssetLocations();
        renderer->MarkDirty();
    };
    editorState->OnSceneLoadRequested = [&](const std::string&)
    {
        gameBuildManager->Stop();
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
    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        char buf[256];
        sprintf_s(buf, "[OnResize] Called with w=%u, h=%u\n", w, h);
        OutputDebugStringA(buf);
        
        renderer->Resize(w, h);
        uiBackend->Resize(w, h);
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
        uiBackend->ClearFocus();
        renderer->MarkDirty();
    };
    window->OnApplicationFocusLost = [&]()
    {
        uiBackend->ClearFocus();
        renderer->MarkDirty();
    };
    window->OnApplicationFocusGained = [&]()
    {
        editorState->CheckAssetLocations();
        renderer->MarkDirty();
    };
    OutputDebugStringA("[Main] WndProcHook callback set\n");

    OutputDebugStringA("[Main] Setting OnUpdate callback...\n");
    std::string displayedWindowTitle;
    Engine::Editor::EditorFrameProfiler frameProfiler(
        scene, renderer, autoProfileMmdPlay);
    uint32_t autoProfileTick = 0;
    window->OnUpdate = [&]()
    {
        using Clock = std::chrono::steady_clock;
        const auto frameBegin = Clock::now();
        editorFramePacer.Wait(editorState->GetEditorMaxFps());
        const auto afterPacing = Clock::now();
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
        const bool wasBuilding = gameBuildManager->IsBuilding();
        const bool wasHotReloadBusy = hotReload->IsBusy();
        Engine::Editor::PostBuildAction postBuildAction;
        if (autoProfileMmdPlay && autoProfileTick == 30u)
            gameBuildManager->PlayInEditor();
        gameBuildManager->Update(playState, postBuildAction);
        hotReload->Update(window->IsFocused());
        const auto afterBackground = Clock::now();
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
            scene->Start();
        }

        // Tick game objects while playing
        if (playState == Engine::Editor::PlayState::Playing &&
            !waitingForPlayDebugger)
        {
            // Physics bounds its own fixed-step work. Keep animation on wall
            // time so a slow render frame does not slow down playback.
            scene->Update(std::max(dt, 0.f));
            Engine::Core::SceneManager::ProcessPendingSceneLoad();
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
                for (const auto& object : previewScene->GetObjects())
                {
                    if (!object || !object->IsEnabledInHierarchy()) continue;
                    if (auto* emitter = object->GetComponent<Engine::Components::ParticleEmitter>();
                        emitter && emitter->IsEditorPreviewRunning())
                    {
                        emitter->Advance(std::max(dt, 0.f),
                            object->transform.GetWorldMatrix());
                        previewAdvanced = true;
                    }
                    if (auto* animation = object->GetComponent<Engine::Components::AnimationManager>();
                        animation && animation->IsEditorPreviewRunning())
                    {
                        animation->Tick(std::max(dt, 0.f));
                        previewAdvanced = true;
                    }
                }
            }
            if (previewAdvanced) renderer->MarkDirty();
        }
        const auto afterSceneUpdate = Clock::now();

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
        uint32_t submittedShadowDraws = 0;

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

        // Render frame
        renderer->RenderIfNeeded([&]()
        {
            const auto drawBegin = Clock::now();
            renderEntryMilliseconds = std::chrono::duration<double, std::milli>(
                drawBegin - afterSceneUpdate).count();
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
                            panelScene->PrepareRenderFrame();
                    continue;
                }
                if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(panel.get()))
                {
                    if (!gameView->IsRenderVisible()) continue;
                    if (Engine::Scene::Scene* panelScene = gameView->GetScene())
                        if (preparedScenes.insert(panelScene).second)
                            panelScene->PrepareRenderFrame();
                }
            }
            const auto afterPrepare = Clock::now();
            if (auto* provider = renderer->GetGraphicsProvider())
                if (auto* factory = provider->GetContextFactory())
                {
                    factory->SetCommandBuffer(renderer->GetCurrentCommandBuffer());
                    auto computeContext = factory->CreateContext();
                    for (Engine::Scene::Scene* prepared : preparedScenes)
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
                
                void* cmdList = renderer->GetCurrentCommandBuffer();
                void* rtvHandle = renderer->GetCurrentRenderTargetHandle();
                
                const auto viewBegin = Clock::now();
                view->Render(cmdList, rtvHandle,
                    [&, view](void* cmd)
                    {
                        const auto begin = Clock::now();
                        view->Render3D(cmd);
                        mainSubmitMilliseconds += std::chrono::duration<double,
                            std::milli>(Clock::now() - begin).count();
                    },
                    [&, view](void* cmd)
                    {
                        const auto begin = Clock::now();
                        view->RenderShadow3D(cmd);
                        shadowSubmitMilliseconds += std::chrono::duration<double,
                            std::milli>(Clock::now() - begin).count();
                    });
                viewMilliseconds += std::chrono::duration<double,
                    std::milli>(Clock::now() - viewBegin).count();
                ++renderedViews;
                if (auto* sceneView = dynamic_cast<Engine::Editor::SceneView*>(view))
                {
                    ++renderedSceneViews;
                    if (auto* panelScene = sceneView->GetScene())
                        submittedShadowDraws += panelScene->GetShadowDebugSnapshot()
                            .submittedCasterDrawCount;
                }
                else if (auto* gameView = dynamic_cast<Engine::Editor::GameView*>(view))
                {
                    ++renderedGameViews;
                    if (auto* panelScene = gameView->GetScene())
                        submittedShadowDraws += panelScene->GetShadowDebugSnapshot()
                            .submittedCasterDrawCount;
                }
            }

            // Render UI
            const auto beforeUi = Clock::now();
            uiBackend->DrawEditor(*editorState, playState, gameBuildManager.get());
            editorUiMilliseconds = std::chrono::duration<double, std::milli>(
                Clock::now() - beforeUi).count();
            for (auto& panel : editorState->GetPanels())
                if (auto* view = dynamic_cast<Engine::Editor::View*>(panel.get()))
                {
                    if (!view->ConsumeVisibilityChanged()) continue;
                    // The tab switch happened during UI drawing, after the
                    // offscreen passes. Render the newly visible tab next frame.
                    renderer->MarkDirty();
                    break;
                }
        });
        const auto afterRender = Clock::now();

        // Scene requests made while drawing the UI now have a presented frame
        // with the loading overlay before deserialization blocks this thread.
        editorState->ProcessPendingSceneLoad();
        const auto afterPendingLoad = Clock::now();
        if (frameProfiler)
        {
            Engine::Editor::EditorFrameProfileSample sample{};
            sample.frameBegin = frameBegin;
            sample.afterPacing = afterPacing;
            sample.afterBackground = afterBackground;
            sample.afterSceneUpdate = afterSceneUpdate;
            sample.afterRender = afterRender;
            sample.afterPendingLoad = afterPendingLoad;
            sample.prepareMilliseconds = prepareMilliseconds;
            sample.renderEntryMilliseconds = renderEntryMilliseconds;
            sample.clearMilliseconds = clearMilliseconds;
            sample.scenePrepareMilliseconds = scenePrepareMilliseconds;
            sample.viewMilliseconds = viewMilliseconds;
            sample.shadowSubmitMilliseconds = shadowSubmitMilliseconds;
            sample.mainSubmitMilliseconds = mainSubmitMilliseconds;
            sample.editorUiMilliseconds = editorUiMilliseconds;
            sample.renderedViews = renderedViews;
            sample.renderedSceneViews = renderedSceneViews;
            sample.renderedGameViews = renderedGameViews;
            sample.submittedShadowDraws = submittedShadowDraws;
            sample.deltaSeconds = dt;
            sample.playing = playState == Engine::Editor::PlayState::Playing;
            frameProfiler.Record(sample, scene, renderer);
        }

        // Avoid spinning the editor loop while idle. Window input wakes this
        // wait immediately; the short timeout also services asynchronous
        // directory notifications and other background state promptly.
        if (!renderer->IsDirty() && !needsContinuousRendering())
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
        if (autoProfileMmdPlay && ++autoProfileTick == 30u + autoProfilePlayFrames)
        {
            gameBuildManager->Stop();
            PostMessageW(window->GetHWND(), WM_CLOSE, 0, 0);
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
    gameBuildManager.reset();
    hotReload.reset();
    editorState.reset();
    uiBackend.reset();

    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}
