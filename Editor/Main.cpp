#include "../Engine/Core/Window.h"
#include "../Engine/Renderers/Editor/DX12EditorRenderer.h"
#include "imgui_internal.h"  // DockBuilder API

// ImGui Win32 WndProc handler — intentionally not declared in imgui_impl_win32.h
// to avoid dragging <windows.h> into that header. Forward-declared here per imgui docs.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// WinMain – Editor entry point
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

    // Resize: update the swap chain then force a redraw.
    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);
        renderer->MarkDirty();
    };

    // Hook ImGui into WndProc
    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };

    // OnUpdate is called every iteration of the Window message loop.
    // RenderIfNeeded() is a no-op when nothing has changed, so the GPU stays
    // idle between interactions. The drawFn lambda is where editor panels,
    // scene views, and property widgets will eventually be drawn.
    window->OnUpdate = [&]() {
        renderer->MarkDirty();   // editor always redraws — ImGui is interactive
        renderer->RenderIfNeeded([&]() {
            renderer->Clear(0.18f, 0.18f, 0.18f);

            ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // Build the default layout once — only when no saved layout exists.
            if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr ||
                ImGui::DockBuilderGetNode(dockspaceId)->IsLeafNode())
            {
                ImGui::DockBuilderRemoveNode(dockspaceId);
                ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                ImGuiID left, center, right;
                // Split off the left 20%
                ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left,  0.20f, &left,   &center);
                // Split the remaining 80% — properties gets ~25% of total (31% of remainder)
                ImGui::DockBuilderSplitNode(center,      ImGuiDir_Right, 0.31f, &right,  &center);

                ImGui::DockBuilderDockWindow("Hierarchy",    left);
                ImGui::DockBuilderDockWindow("Scene",        center);
                ImGui::DockBuilderDockWindow("Properties",   right);
                ImGui::DockBuilderFinish(dockspaceId);
            }

            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            ImGui::Begin("Hierarchy");
            ImGui::End();

            ImGui::Begin("Properties");
            ImGui::End();

            ImGui::Begin("Scene");
            ImGui::End();
        });
    };

    return window->Run();
}
