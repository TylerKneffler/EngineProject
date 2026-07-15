#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/View/View.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/EditorUI.h"
#include "Engine/Editor/ProjectLauncher.h"
#include <filesystem>
#include <shellapi.h>

// Fallback for IntelliSense
#ifndef PROJECT_FILE
#define PROJECT_FILE ""
#endif
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
    ProjectLoader projectLoader;
    ProjectSettings projectSettings;
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
            ProjectLauncher::RememberProject(projectFile);
            break;
        }
        catch (const std::exception& error)
        {
            MessageBoxA(nullptr, error.what(), "Could not open project", MB_OK | MB_ICONERROR);
            projectFile.clear();
        }
    }

    // Create editor state and UI
    OutputDebugStringA("[Main] Creating EditorState...\n");
    auto editorState = std::make_unique<EditorState>(hInstance, projectSettings, projectFile);
    OutputDebugStringA("[Main] EditorState created, calling Init...\n");
    if (!editorState->Init())
        return 1;
    OutputDebugStringA("[Main] EditorState initialized\n");

    // Load default scene if specified
    if (!projectSettings.defaultScene.empty())
    {
        OutputDebugStringA(("[Main] Loading default scene: " + projectSettings.defaultScene + "\n").c_str());
        editorState->LoadScene(projectSettings.defaultScene);
    }

    OutputDebugStringA("[Main] Creating EditorUI...\n");
    auto editorUI = std::make_unique<EditorUI>(editorState.get());
    OutputDebugStringA("[Main] EditorUI created\n");
    
    OutputDebugStringA("[Main] Creating GameBuildManager...\n");
    auto gameBuildManager = std::make_unique<GameBuildManager>(editorState->GetConsole(), projectFile);
    OutputDebugStringA("[Main] GameBuildManager created\n");
    editorUI->SetGameBuildManager(gameBuildManager.get());
    OutputDebugStringA("[Main] Getting window, renderer, scene...\n");
    Window* window = editorState->GetWindow();
    IEditorRenderer* renderer = editorState->GetRenderer();
    Scene* scene = editorState->GetScene();
    
    if (!window)
    {
        OutputDebugStringA("[Main] ERROR: Window is null!\n");
        return 1;
    }
    if (!renderer)
    {
        OutputDebugStringA("[Main] ERROR: Renderer is null!\n");
        return 1;
    }
    if (!scene)
    {
        OutputDebugStringA("[Main] ERROR: Scene is null!\n");
        return 1;
    }
    
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
    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };
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
            editorUI->Render(playState);
        });
    };
    OutputDebugStringA("[Main] OnUpdate callback set\n");

    // Now that all callbacks are set up, show the window
    OutputDebugStringA("[Main] Showing window...\n");
    window->Show();
    OutputDebugStringA("[Main] Window shown, entering message loop...\n");

    int result = window->Run();

    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}

