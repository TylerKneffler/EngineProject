#include "Core/Window.h"
#include "Core/ProjectLoader.h"
#include "Core/SceneManager.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/DX12/DX12EditorRenderer.h"
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
    auto editorState = std::make_unique<EditorState>(hInstance, projectSettings);
    if (!editorState->Init())
        return 1;

    auto editorUI = std::make_unique<EditorUI>(editorState.get());
    auto gameBuildManager = std::make_unique<GameBuildManager>(editorState->GetConsole());

    Window* window = editorState->GetWindow();
    IEditorRenderer* renderer = editorState->GetRenderer();
    Scene* scene = editorState->GetScene();

    // For D3D12-specific operations, cast to concrete renderer
    auto* dx12Renderer = dynamic_cast<DX12EditorRenderer*>(renderer);
    if (!dx12Renderer)
    {
        OutputDebugStringA("Error: Editor requires a D3D12-based renderer\n");
        return 1;
    }

    // Frame timing
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    // Play state
    PlayState playState = PlayState::Stopped;

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);
        for (auto& panel : editorState->GetPanels())
        {
            if (panel->NeedsRender())
            {
                class View* view = reinterpret_cast<class View*>(panel.get());
                view->Resize(dx12Renderer->GetDevice(), w, h);
            }
        }
        renderer->MarkDirty();
    };

    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };

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
            editorState->SetHasUnsavedChanges(true);
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
                if (!panel->NeedsRender() || !panel->IsOpen()) continue;
                class View* view = reinterpret_cast<class View*>(panel.get());
                
                auto cmdList = dx12Renderer->GetCommandList();
                auto rtv = dx12Renderer->GetCurrentRTV();
                
                view->Render(cmdList, &rtv,
                    [view](void* cmd) { view->Render3D(cmd); });
            }

            // Render UI
            editorUI->Render(playState);
        });
    };

    int result = window->Run();

    return result;
}

