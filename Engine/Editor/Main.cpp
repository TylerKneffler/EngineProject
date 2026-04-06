#include "Core/Window.h"
#include "Core/Renderers/Editor/DX12EditorRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "View/Views/SceneView.h"
#include "View/Views/GameView.h"
#include "View/Views/HierarchyView.h"
#include "View/Views/PropertiesView.h"
#include "imgui_internal.h"  // DockBuilder API
#include "Scripts/Rotate.h"

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
#endif

// ImGui Win32 WndProc handler — intentionally not declared in imgui_impl_win32.h
// to avoid dragging <windows.h> into that header. Forward-declared here per imgui docs.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Play state — controls whether game lifecycle (Start/Update) is running.
// ---------------------------------------------------------------------------
enum class PlayState { Stopped, Building, BuildFailed, Playing, Paused };

// ---------------------------------------------------------------------------
// Launches  cmake --build <dir> --config Debug --target Game --parallel
// in a detached background process.  Returns the process HANDLE (caller must
// CloseHandle it) or nullptr on failure.
// ---------------------------------------------------------------------------
static HANDLE StartGameBuild()
{
    std::string cmd =
        "cmake --build \"" ENGINE_BUILD_DIR
        "\" --config Debug --target Game --parallel";

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, buf.data(),
                        nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW,   // no console flash
                        nullptr, nullptr, &si, &pi))
        return nullptr;

    CloseHandle(pi.hThread); // we only need the process handle
    return pi.hProcess;
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

    // Game viewport — renders from the game camera (SRV slot 2).
    auto [gameSrvCpu, gameSrvGpu] = renderer->GetSrvSlot(2);
    GameView gameViewport;
    gameViewport.Init(device,
                      window->GetWidth(), window->GetHeight(),
                      gameSrvCpu, gameSrvGpu,
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
    cubeObj->AddComponent<Rotate>(); 
    mesh->LoadFromFile(ASSETS_PATH "cube.obj");
    mesh->CreateBuffer(device);

    // High-resolution timer for frame delta time.
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    // Play state — tracks whether the game simulation is running.
    PlayState playState    = PlayState::Stopped;
    HANDLE    hBuildProcess = nullptr;  // valid only while playState == Building

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);           // flushes GPU before recreating swap chain
        sceneViewport.Resize(renderer->GetDevice(), w, h);
        gameViewport.Resize(renderer->GetDevice(), w, h);
        renderer->MarkDirty();
    };

    window->WndProcHook = [](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return ImGui_ImplWin32_WndProcHandler(h, m, w, l) != 0;
    };

    window->OnUpdate = [&]()
    {
        // Compute delta time — will be passed to Update(dt) once that signature is added.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                 / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;
        (void)dt;

        // Poll the background cmake build process.
        if (playState == PlayState::Building && hBuildProcess)
        {
            if (WaitForSingleObject(hBuildProcess, 0) != WAIT_TIMEOUT)
            {
                DWORD exitCode = 1;
                GetExitCodeProcess(hBuildProcess, &exitCode);
                CloseHandle(hBuildProcess);
                hBuildProcess = nullptr;

                if (exitCode == 0)
                {
                    // Build succeeded — kick off the in-process simulation.
                    playState = PlayState::Playing;
                    for (const auto& obj : scene.GetObjects())
                        obj->Start();
                }
                else
                {
                    playState = PlayState::BuildFailed;
                }
            }
        }

        // Tick game objects while the simulation is running.
        if (playState == PlayState::Playing)
        {
            for (const auto& obj : scene.GetObjects())
                obj->Update();
        }

        renderer->MarkDirty();
        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(0.18f, 0.18f, 0.18f);

            // Render 3-D scene into the scene offscreen texture.
            sceneViewport.Render(renderer->GetCommandList(), renderer->GetCurrentRTV(),
                [&](ID3D12GraphicsCommandList* cmd)
                {
                    scene.Render(cmd, sceneViewport.GetAspect());
                });

            // Render game camera view into the game offscreen texture.
            gameViewport.Render(renderer->GetCommandList(), renderer->GetCurrentRTV(),
                [&](ID3D12GraphicsCommandList* cmd)
                {
                    scene.Render(cmd, gameViewport.GetAspect(),
                                 scene.FindGameCamera());
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
                ImGui::DockBuilderDockWindow("Game",       center); // tabs with Scene
                ImGui::DockBuilderDockWindow("Properties", right);
                ImGui::DockBuilderFinish(dockspaceId);
            }

            // ---------------------------------------------------------------
            // Main menu bar — File menu on the left, play controls in center.
            // ---------------------------------------------------------------
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                    ImGui::EndMenu();
                }

                const float singleBtnW = 60.f;
                const float doubleBtnW = singleBtnW * 2.f + 4.f;
                const float barW       = ImGui::GetWindowWidth();

                if (playState == PlayState::Stopped || playState == PlayState::BuildFailed)
                {
                    ImGui::SetCursorPosX((barW - singleBtnW) * 0.5f);
                    if (ImGui::Button("Play", { singleBtnW, 0.f }))
                    {
                        hBuildProcess = StartGameBuild();
                        playState = hBuildProcess ? PlayState::Building
                                                  : PlayState::BuildFailed;
                    }
                    if (playState == PlayState::BuildFailed)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored({ 1.f, 0.3f, 0.3f, 1.f }, "Build failed");
                    }
                }
                else if (playState == PlayState::Building)
                {
                    // Animated spinner while cmake runs.
                    static const char* kSpinner[] = { "|", "/", "-", "\\" };
                    int frame = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
                    ImGui::SetCursorPosX((barW - 120.f) * 0.5f);
                    ImGui::Text("Building... %s", kSpinner[frame]);
                    ImGui::SameLine(0.f, 8.f);
                    if (ImGui::Button("Cancel", { 56.f, 0.f }))
                    {
                        TerminateProcess(hBuildProcess, 1);
                        CloseHandle(hBuildProcess);
                        hBuildProcess = nullptr;
                        playState = PlayState::Stopped;
                    }
                }
                else if (playState == PlayState::Playing)
                {
                    ImGui::SetCursorPosX((barW - doubleBtnW) * 0.5f);
                    if (ImGui::Button("Pause", { singleBtnW, 0.f }))
                        playState = PlayState::Paused;
                    ImGui::SameLine(0.f, 4.f);
                    if (ImGui::Button("Stop", { singleBtnW, 0.f }))
                        playState = PlayState::Stopped;
                }
                else // Paused
                {
                    ImGui::SetCursorPosX((barW - doubleBtnW) * 0.5f);
                    if (ImGui::Button("Resume", { singleBtnW, 0.f }))
                        playState = PlayState::Playing;
                    ImGui::SameLine(0.f, 4.f);
                    if (ImGui::Button("Stop", { singleBtnW, 0.f }))
                        playState = PlayState::Stopped;
                }

                ImGui::EndMainMenuBar();
            }

            hierarchy.DrawPanel();
            propertiesView.DrawPanel();
            sceneViewport.DrawPanel();
            gameViewport.DrawPanel();
        });
    };

    int result = window->Run();

    // Ensure the build process is cleaned up if the window is closed mid-build.
    if (hBuildProcess)
    {
        TerminateProcess(hBuildProcess, 1);
        CloseHandle(hBuildProcess);
    }

    return result;
}
