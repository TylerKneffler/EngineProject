#include "Core/Window.h"
#include "Core/Renderers/Editor/DX12EditorRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "View/Views/SceneView.h"
#include "View/Views/HierarchyView.h"
#include "View/Views/PropertiesView.h"
#include "imgui_internal.h"  // DockBuilder API

// Fallback for IntelliSense — CMake overrides this with the real absolute path.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif

// ImGui Win32 WndProc handler — intentionally not declared in imgui_impl_win32.h
// to avoid dragging <windows.h> into that header. Forward-declared here per imgui docs.
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
    auto window   = std::make_unique<Window>(hInstance, L"Engine Editor", 1280, 720);
    auto renderer = std::make_unique<DX12EditorRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    ID3D12Device* device = renderer->GetDevice();

    // Scene must be initialised before the viewport so it can be passed in.
    Scene scene;
    scene.Init(device);

    // Scene viewport — renders 3-D content into an offscreen texture (SRV slot 1).
    auto [srvCpu, srvGpu] = renderer->GetSrvSlot(1);
    SceneView sceneViewport;
    sceneViewport.Init(device,
                       window->GetWidth(), window->GetHeight(),
                       srvCpu, srvGpu,
                       &scene);

    HierarchyView hierarchy;
    hierarchy.Init(&scene);
    PropertiesView propertiesView;
    hierarchy.OnSelectionChanged = [&](Object* obj) {
        propertiesView.SetSelectedObject(obj);
        scene.SetSelectedObject(obj);
    };

    // Add the cube as a managed game object.
    Object* cubeObj = scene.AddObject("Cube");
    Mesh*   mesh    = cubeObj->AddComponent<Mesh>();
    cubeObj->AddComponent<Material>();
    mesh->LoadFromFile(ASSETS_PATH "cube.obj");
    mesh->CreateBuffer(device);

    // High-resolution timer for frame delta time.
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);           // flushes GPU before recreating swap chain
        sceneViewport.Resize(renderer->GetDevice(), w, h);
        renderer->MarkDirty();
    };

    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };

    window->OnUpdate = [&]()
    {
        // Compute delta time.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;

        renderer->MarkDirty();
        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(0.18f, 0.18f, 0.18f);

            // Render 3-D scene into the offscreen texture before any ImGui calls
            // so the texture is in SRV state when ImGui samples it.
            sceneViewport.Render(renderer->GetCommandList(), renderer->GetCurrentRTV(),
                [&](ID3D12GraphicsCommandList* cmd)
                {
                    scene.Render(cmd, sceneViewport.GetAspect());
                });

            ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // Build the default layout once — only when no saved layout exists.
            if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr ||
                ImGui::DockBuilderGetNode(dockspaceId)->IsLeafNode())
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID left, center, right;
                ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left,  0.20f, &left,   &center);
                ImGui::DockBuilderSplitNode(center,      ImGuiDir_Right, 0.31f, &right,  &center);

                ImGui::DockBuilderDockWindow("HierarchyView",  left);
                ImGui::DockBuilderDockWindow("Scene",      center);
                ImGui::DockBuilderDockWindow("Properties", right);
                ImGui::DockBuilderFinish(dockspaceId);
            }

            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            hierarchy.DrawPanel();
            propertiesView.DrawPanel();
            sceneViewport.DrawPanel();
        });
    };

    return window->Run();
}
