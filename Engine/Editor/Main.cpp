#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/View/View.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/ProjectLauncher.h"
#include "Engine/Editor/UI/IEditorUiBackend.h"
#include <filesystem>
#include <fstream>
#include <shellapi.h>

// Fallback for IntelliSense
#ifndef PROJECT_FILE
#define PROJECT_FILE ""
#endif
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif
#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
#endif

namespace
{
    void WriteStartupLog(const std::string& message, bool reset = false)
    {
        std::ofstream log("editor-startup.log", reset ? std::ios::trunc : std::ios::app);
        if (log)
            log << message << '\n';
    }

    bool IsEngineDevelopmentDirectory()
    {
        std::error_code error;
        const auto current = std::filesystem::weakly_canonical(
            std::filesystem::current_path(), error);
        if (error) return false;
        const auto engineRoot = std::filesystem::weakly_canonical(
            std::filesystem::path(ENGINE_ROOT_PATH), error);
        return !error && current == engineRoot &&
            std::filesystem::is_directory(engineRoot / "Engine" / "Core" / "Assets") &&
            std::filesystem::is_regular_file(engineRoot / "CMakeLists.txt");
    }

    ProjectSettings CreateEngineDevelopmentSettings()
    {
        ProjectSettings settings{};
        settings.name = "Engine Sandbox";
        settings.version = "Development";
        settings.description = "Built-in project-free engine development environment";
        settings.engineDirectory = std::filesystem::path(ENGINE_ROOT_PATH).string();
        settings.assetsDirectory = std::filesystem::path(ENGINE_ASSETS_PATH).string();
        settings.sceneDirectory =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes").string();
        settings.scriptsDirectory =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scripts").string();
        settings.shadersDirectory = std::filesystem::path(ENGINE_SHADERS_PATH).string();
        settings.buildDirectory = std::filesystem::path(ENGINE_BUILD_DIR).string();
        settings.defaultScene =
            (std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes" / "default.scene").string();
        settings.viewportWidth = 1280;
        settings.viewportHeight = 720;
        settings.leftPanelWidth = 0.20f;
        settings.rightPanelWidth = 0.31f;
        settings.leftPanelTabs = { "HierarchyView", "Assets" };
        settings.centerPanelTabs = { "Scene", "Game" };
        settings.rightPanelTabs = { "Properties" };
        settings.renderingAPI = "DirectX11";
        settings.editorRenderingAPI = "DirectX11";
        settings.gameRenderingAPI = "DirectX11";
        settings.clearColor = { 0.18f, 0.18f, 0.18f, 1.f };
        settings.targetFramerate = 60;
        settings.aspectRatioMode = ProjectSettings::AspectRatioMode::Free;
        return settings;
    }
}

// ---------------------------------------------------------------------------
// WinMain — Editor entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       /*nShowCmd*/)
{
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WriteStartupLog("Editor startup", true);

    auto resolveProject = []() -> std::string
    {
        int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (arguments)
        {
            if (argumentCount > 1 && std::filesystem::exists(arguments[1]))
            {
                std::string path = std::filesystem::absolute(arguments[1]).string();
                LocalFree(arguments);
                return path;
            }
            LocalFree(arguments);
        }

        if (std::string(PROJECT_FILE).size() > 0 && std::filesystem::exists(PROJECT_FILE))
            return std::filesystem::absolute(PROJECT_FILE).string();

        std::vector<std::string> projects;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path()))
            if (entry.is_regular_file() && entry.path().extension() == ".proj")
                projects.push_back(std::filesystem::absolute(entry.path()).string());
        if (projects.size() == 1)
            return projects.front();
        for (const auto& project : projects)
            ProjectLauncher::RememberProject(project);
        return {};
    };

    std::string projectFile = resolveProject();
    auto hasArgument = [](const wchar_t* expected) -> bool
    {
        int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        bool found = false;
        if (arguments)
        {
            for (int i = 1; i < argumentCount; ++i)
                if (std::wstring(arguments[i]) == expected)
                {
                    found = true;
                    break;
                }
            LocalFree(arguments);
        }
        return found;
    };
    auto rendererOverride = []() -> std::string
    {
        int argumentCount = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        std::string value;
        if (arguments)
        {
            for (int i = 1; i + 1 < argumentCount; ++i)
                if (std::wstring(arguments[i]) == L"--editor-renderer")
                {
                    value = std::filesystem::path(arguments[i + 1]).string();
                    break;
                }
            LocalFree(arguments);
        }
        return value;
    };
    if (hasArgument(L"--project-hub"))
        projectFile.clear();
    ProjectLoader projectLoader;
    ProjectSettings projectSettings;
    const bool engineDevelopmentMode = projectFile.empty() &&
        !hasArgument(L"--project-hub") && IsEngineDevelopmentDirectory();
    if (engineDevelopmentMode)
    {
        projectSettings = CreateEngineDevelopmentSettings();
        const std::string overrideApi = rendererOverride();
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
                projectFile = ProjectLauncher::Run(hInstance);
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
                const std::string overrideApi = rendererOverride();
                if (!overrideApi.empty())
                    projectSettings.editorRenderingAPI = overrideApi;
                WriteStartupLog("Project: " + projectFile);
                WriteStartupLog("Editor renderer: " + projectSettings.editorRenderingAPI);
                ProjectLauncher::RememberProject(projectFile);
                break;
            }
            catch (const std::exception& error)
            {
                MessageBoxA(nullptr, error.what(), "Could not open project", MB_OK | MB_ICONERROR);
                projectFile.clear();
            }
        }
    }

    // Create editor state and UI
    OutputDebugStringA("[Main] Creating EditorState...\n");
    auto editorState = std::make_unique<EditorState>(hInstance, projectSettings, projectFile);
    OutputDebugStringA("[Main] EditorState created, calling Init...\n");
    if (!editorState->Init())
    {
        WriteStartupLog("Editor initialization failed with " + projectSettings.editorRenderingAPI);
        std::string fallbackReason;
        if (projectSettings.editorRenderingAPI != "DirectX11" &&
            RendererFactory::IsRendererAvailable("DirectX11", &fallbackReason))
        {
            OutputDebugStringA(("[Main] " + projectSettings.editorRenderingAPI +
                " editor initialization failed; retrying with DirectX11.\n").c_str());
            projectSettings.editorRenderingAPI = "DirectX11";
            editorState = std::make_unique<EditorState>(hInstance, projectSettings, projectFile);
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
    OutputDebugStringA("[Main] EditorState initialized\n");

    Window* window = editorState->GetWindow();
    IEditorRenderer* renderer = editorState->GetRenderer();
    Scene* scene = editorState->GetScene();
    if (!window || !renderer || !scene)
    {
        WriteStartupLog("Editor did not create all required core components");
        return 1;
    }

    auto uiBackend = CreateEditorUiBackend();
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

    // Load default scene if specified
    if (!projectSettings.defaultScene.empty())
    {
        OutputDebugStringA(("[Main] Loading default scene: " + projectSettings.defaultScene + "\n").c_str());
        editorState->LoadScene(projectSettings.defaultScene);
    }

    OutputDebugStringA("[Main] Creating GameBuildManager...\n");
    auto gameBuildManager = std::make_unique<GameBuildManager>(
        editorState->GetConsole(), projectFile, projectSettings);
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

    gameBuildManager->OnPlayStart = [&]()
    {
        editorState->CapturePlayModeScene();
        for (const auto& obj : scene->GetObjects())
            obj->Start();
    };
    gameBuildManager->OnPlayStop = [&]()
    {
        editorState->RestorePlayModeScene();
    };

    // Frame timing
    OutputDebugStringA("[Main] Setting up frame timing...\n");
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    // Play state
    PlayState playState = PlayState::Stopped;
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
                class View* view = reinterpret_cast<class View*>(panel.get());
                view->Resize(renderer->GetNativeDeviceHandle(), w, h);
            }
        }
        renderer->MarkDirty();
    };
    OutputDebugStringA("[Main] OnResize callback set\n");

    OutputDebugStringA("[Main] Setting WndProcHook callback...\n");
    window->WndProcHook = [&](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return uiBackend->HandleMessage(h, m, w, l);
    };
    window->OnInputBegin = [&]() { uiBackend->BeginInput(); };
    window->OnInputEnd = [&]() { uiBackend->EndInput(); };
    OutputDebugStringA("[Main] WndProcHook callback set\n");

    OutputDebugStringA("[Main] Setting OnUpdate callback...\n");
    window->OnUpdate = [&]()
    {
        // Update delta time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;
        (void)dt;

        // Update build manager
        PostBuildAction postBuildAction;
        gameBuildManager->Update(playState, postBuildAction);

        // Tick game objects while playing
        if (playState == PlayState::Playing)
        {
            for (const auto& obj : scene->GetObjects())
                obj->Update();
        }

        renderer->MarkDirty();

        // Update window title
        if (engineDevelopmentMode)
            SetWindowTextW(window->GetHWND(), editorState->HasUnsavedChanges()
                ? L"Engine Editor - Engine Sandbox *" : L"Engine Editor - Engine Sandbox");
        else
            SetWindowTextW(window->GetHWND(),
                           editorState->HasUnsavedChanges() ? L"Engine Editor *" : L"Engine Editor");

        // Render frame
        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(projectSettings.clearColor.r, projectSettings.clearColor.g, projectSettings.clearColor.b);

            // Render 3D panels
            for (auto& panel : editorState->GetPanels())
            {
                if (!panel) continue;
                if (!panel->NeedsRender() || !panel->IsOpen()) continue;
                class View* view = dynamic_cast<class View*>(panel.get());
                if (!view) continue;
                
                void* cmdList = renderer->GetCurrentCommandBuffer();
                void* rtvHandle = renderer->GetCurrentRenderTargetHandle();
                
                view->Render(cmdList, rtvHandle,
                    [view](void* cmd) { view->Render3D(cmd); });
            }

            // Render UI
            uiBackend->DrawEditor(*editorState, playState, gameBuildManager.get());
        });
    };
    OutputDebugStringA("[Main] OnUpdate callback set\n");

    // Now that all callbacks are set up, show the window
    OutputDebugStringA("[Main] Showing window...\n");
    window->Show();
    OutputDebugStringA("[Main] Window shown, entering message loop...\n");

    int result = window->Run();

    // Panels release UI texture registrations while the selected package is
    // still alive. The renderer itself is owned by EditorState.
    gameBuildManager.reset();
    editorState.reset();
    uiBackend.reset();

    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}

