#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/View/View.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/EditorUI.h"

// Fallback for IntelliSense
#ifndef PROJECT_FILE
#define PROJECT_FILE "Example_Proj.proj"
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
    // Load project configuration
    ProjectLoader projectLoader;
    ProjectSettings projectSettings;
    try
    {
        projectSettings = projectLoader.LoadProject(PROJECT_FILE);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Warning: Failed to load project file: " << e.what() << std::endl;
        projectSettings.assetsDirectory = ASSETS_PATH;
        projectSettings.viewportWidth = 1280;
        projectSettings.viewportHeight = 720;
        projectSettings.clearColor = glm::vec4(0.18f, 0.18f, 0.18f, 1.0f);
    }

    // Create editor state and UI
    OutputDebugStringA("[Main] Creating EditorState...\n");
    auto editorState = std::make_unique<EditorState>(hInstance, projectSettings);
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
    auto gameBuildManager = std::make_unique<GameBuildManager>(editorState->GetConsole());
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
        for (const auto& obj : scene->GetObjects())
            obj->Start();
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
        
        sprintf_s(buf, "[OnResize] renderer pointer: %p\n", (void*)renderer);
        OutputDebugStringA(buf);
        
        OutputDebugStringA("[OnResize] Calling renderer->Resize...\n");
        renderer->Resize(w, h);
        OutputDebugStringA("[OnResize] renderer->Resize completed\n");
        
        OutputDebugStringA("[OnResize] Iterating panels...\n");
        for (auto& panel : editorState->GetPanels())
        {
            if (panel->NeedsRender())
            {
                OutputDebugStringA("[OnResize] Resizing a panel view...\n");
                class View* view = reinterpret_cast<class View*>(panel.get());
                view->Resize(renderer->GetNativeDeviceHandle(), w, h);
            }
        }
        OutputDebugStringA("[OnResize] Panels resized\n");
        
        OutputDebugStringA("[OnResize] Calling renderer->MarkDirty...\n");
        renderer->MarkDirty();
        OutputDebugStringA("[OnResize] renderer->MarkDirty completed\n");
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
        OutputDebugStringA("[OnUpdate] Entry\n");
        // Update delta time
        OutputDebugStringA("[OnUpdate] Querying performance counter\n");
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        OutputDebugStringA("[OnUpdate] Calculating delta time\n");
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;
        (void)dt;

        // Update build manager
        OutputDebugStringA("[OnUpdate] Updating build manager\n");
        PostBuildAction postBuildAction;
        gameBuildManager->Update(playState, postBuildAction);
        OutputDebugStringA("[OnUpdate] Build manager updated\n");

        // Tick game objects while playing
        OutputDebugStringA("[OnUpdate] Checking playState\n");
        if (playState == PlayState::Playing)
        {
            OutputDebugStringA("[OnUpdate] PlayState is Playing, updating objects\n");
            for (const auto& obj : scene->GetObjects())
                obj->Update();
            editorState->SetHasUnsavedChanges(true);
        }
        OutputDebugStringA("[OnUpdate] After playState check\n");

        OutputDebugStringA("[OnUpdate] Calling renderer->MarkDirty\n");
        renderer->MarkDirty();
        OutputDebugStringA("[OnUpdate] MarkDirty completed\n");

        // Update window title
        OutputDebugStringA("[OnUpdate] Updating window title\n");
        SetWindowTextW(window->GetHWND(),
                       editorState->HasUnsavedChanges() ? L"Engine Editor *" : L"Engine Editor");
        OutputDebugStringA("[OnUpdate] Window title updated\n");

        // Render frame
        OutputDebugStringA("[OnUpdate] Calling RenderIfNeeded\n");
        renderer->RenderIfNeeded([&]()
        {
            OutputDebugStringA("[RenderIfNeeded Lambda] Entry\n");
            OutputDebugStringA("[RenderIfNeeded Lambda] Calling renderer->Clear\n");
            renderer->Clear(projectSettings.clearColor.r, projectSettings.clearColor.g, projectSettings.clearColor.b);
            OutputDebugStringA("[RenderIfNeeded Lambda] Clear completed\n");

            // Render 3D panels
            OutputDebugStringA("[RenderIfNeeded Lambda] Rendering 3D panels\n");
            for (auto& panel : editorState->GetPanels())
            {
                if (!panel) { OutputDebugStringA("[RenderIfNeeded Lambda] NULL panel in list!\n"); continue; }
                OutputDebugStringA(("[RenderIfNeeded Lambda] Panel: " + panel->GetTitle() + " NeedsRender=" + std::to_string(panel->NeedsRender()) + " IsOpen=" + std::to_string(panel->IsOpen()) + "\n").c_str());
                if (!panel->NeedsRender() || !panel->IsOpen()) continue;
                class View* view = dynamic_cast<class View*>(panel.get());
                if (!view) { OutputDebugStringA("[RenderIfNeeded Lambda] dynamic_cast to View failed\n"); continue; }
                
                void* cmdList = renderer->GetCurrentCommandBuffer();
                void* rtvHandle = renderer->GetCurrentRenderTargetHandle();
                
                OutputDebugStringA(("[RenderIfNeeded Lambda] Rendering: " + panel->GetTitle() + "\n").c_str());
                view->Render(cmdList, rtvHandle,
                    [view](void* cmd) { view->Render3D(cmd); });
            }
            OutputDebugStringA("[RenderIfNeeded Lambda] 3D panels done\n");

            // Render UI
            OutputDebugStringA("[RenderIfNeeded Lambda] Calling editorUI->Render\n");
            editorUI->Render(playState);
            OutputDebugStringA("[RenderIfNeeded Lambda] editorUI->Render completed\n");
        });
    };
    OutputDebugStringA("[Main] OnUpdate callback set\n");

    // Now that all callbacks are set up, show the window
    OutputDebugStringA("[Main] Showing window...\n");
    window->Show();
    OutputDebugStringA("[Main] Window shown, entering message loop...\n");

    int result = window->Run();

    return result;
}

