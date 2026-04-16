#include "Core/Window.h"
#include "Core/Renderers/Editor/DX12EditorRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/ProjectLoader.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/ViewFactory.h"
#include "imgui_internal.h"  // DockBuilder API
#include "Scripts/Rotate.h"

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif
#ifndef PROJECT_FILE
#define PROJECT_FILE "Example_Proj.proj"
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

// What to do once a background build completes.
enum class PostBuildAction { PlayInEditor, LaunchStandalone, Nothing };

// ---------------------------------------------------------------------------
// Launches  cmake --build <dir> --config Debug --target Game --parallel
// in a detached background process.  Stdout and stderr are captured via
// an anonymous pipe; the read end is returned via outReadPipe so the caller
// can poll it each frame. Returns the process HANDLE or nullptr on failure.
// ---------------------------------------------------------------------------
static HANDLE StartGameBuild(HANDLE& outReadPipe)
{
    outReadPipe = nullptr;

    // Create an anonymous pipe.  The write end is inherited by the child;
    // the read end stays in the parent and must NOT be inherited.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return nullptr;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0); // parent-only

    std::string cmd =
        "cmake --build \"" ENGINE_BUILD_DIR
        "\" --config Debug --target Game --parallel";

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = nullptr;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, buf.data(),
                        nullptr, nullptr, TRUE,   // bInheritHandles = TRUE
                        CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return nullptr;
    }

    CloseHandle(hWrite); // parent doesn't write; close so ReadFile detects EOF
    CloseHandle(pi.hThread);
    outReadPipe = hRead;
    return pi.hProcess;
}

// ---------------------------------------------------------------------------
// Launches the compiled Game.exe as an independent process (standalone run).
// Returns immediately — does not wait for the game to exit.
// ---------------------------------------------------------------------------
static void LaunchGameExe()
{
    std::string path = std::string(ENGINE_BUILD_DIR) + "/Debug/Game.exe";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (CreateProcessA(nullptr, buf.data(),
                       nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi))
    {
        // We don't track this process — it's standalone.
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
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
    // Load project configuration
    ProjectLoader projectLoader;
    ProjectSettings projectSettings;
    try
    {
        projectSettings = projectLoader.LoadProject(PROJECT_FILE);
    }
    catch (const std::exception& e)
    {
        // If project loading fails, use defaults
        std::cerr << "Warning: Failed to load project file: " << e.what() << std::endl;
        projectSettings.assetsDirectory = ASSETS_PATH;
        projectSettings.viewportWidth = 1280;
        projectSettings.viewportHeight = 720;
        projectSettings.clearColor = glm::vec4(0.18f, 0.18f, 0.18f, 1.0f);
    }

    auto window   = std::make_unique<Window>(hInstance, L"Engine Editor", projectSettings.viewportWidth, projectSettings.viewportHeight);
    auto renderer = std::make_unique<DX12EditorRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    ID3D12Device* device = renderer->GetDevice();

    // Scene must be initialised before the viewport so it can be passed in.
    Scene scene;
    scene.Init(device);

    // Register script component types for scene serialization.
    SceneSerializer::Register("Rotate", []() -> Component* { return new Rotate(); });

    // Track unsaved changes
    bool hasUnsavedChanges = false;
    std::string sceneToLoad;
    bool showUnsavedWarning = false;

    // -----------------------------------------------------------------------
    // View factory — creates and names all editor panels.
    // -----------------------------------------------------------------------
    ViewFactory viewFactory(renderer.get(), &scene, projectSettings);

    // Callbacks shared by all hierarchy / assets panels.
    // (Populated below after creating primary panels.)
    std::vector<std::unique_ptr<IEditorPanel>> panels;

    // Create default layout panels.
    panels.push_back(viewFactory.Create("Scene"));
    panels.push_back(viewFactory.Create("Game"));
    panels.push_back(viewFactory.Create("Hierarchy"));
    panels.push_back(viewFactory.Create("Properties"));
    panels.push_back(viewFactory.Create("Assets"));
    panels.push_back(viewFactory.Create("Console"));

    // Keep a raw pointer to the primary console for build log output.
    auto* primaryConsole    = static_cast<ConsoleView*>(panels[5].get());

    // Wire hierarchy → properties + scene selection for all hierarchy panels.
    auto selectionCallback = [&](Object* obj)
    {
        // Update every PropertiesView in the panel list.
        for (auto& p : panels)
        {
            if (auto* pv = dynamic_cast<PropertiesView*>(p.get()))
                pv->SetSelectedObject(obj);
        }
        scene.SetSelectedObject(obj);
    };
    viewFactory.OnSelectionChanged = selectionCallback;
    static_cast<HierarchyView*>(panels[2].get())->OnSelectionChanged = selectionCallback;

    // Wire assets explorer → scene load for all assets panels.
    auto sceneRequestCallback = [&](const std::string& scenePath)
    {
        if (hasUnsavedChanges)
        {
            sceneToLoad = scenePath;
            showUnsavedWarning = true;
        }
        else
        {
            scene.Load(scenePath);
            hasUnsavedChanges = false;
        }
    };
    viewFactory.OnSceneRequested = sceneRequestCallback;
    static_cast<AssetsExplorerView*>(panels[4].get())->OnSceneRequested = sceneRequestCallback;

    PreferencesView preferencesView;
    preferencesView.Init(projectSettings, PROJECT_FILE);
    preferencesView.OnSettingsChanged = [&]() { hasUnsavedChanges = true; };
    bool showPreferences = false;

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
    PlayState playState     = PlayState::Stopped;
    PostBuildAction postBuildAction = PostBuildAction::Nothing;
    HANDLE    hBuildProcess = nullptr;  // valid only while playState == Building
    HANDLE    hBuildPipe    = nullptr;  // read end of the cmake stdout/stderr pipe
    std::string buildLineBuffer;        // accumulates partial lines from the pipe

    // -----------------------------------------------------------------------
    // Window callbacks
    // -----------------------------------------------------------------------

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);           // flushes GPU before recreating swap chain
        for (auto& panel : panels)
        {
            if (panel->NeedsRender())
                static_cast<View*>(panel.get())->Resize(renderer->GetDevice(), w, h);
        }
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

        // Drain the cmake pipe — read whatever bytes are available this frame.
        if (hBuildPipe)
        {
            DWORD available = 0;
            while (PeekNamedPipe(hBuildPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
            {
                char tmp[512];
                DWORD toRead = available < sizeof(tmp) ? available : sizeof(tmp);
                DWORD bytesRead = 0;
                if (!ReadFile(hBuildPipe, tmp, toRead, &bytesRead, nullptr) || bytesRead == 0)
                    break;

                // Split raw bytes into lines and forward to the console.
                for (DWORD i = 0; i < bytesRead; ++i)
                {
                    if (tmp[i] == '\n')
                    {
                        // Strip trailing \r if present.
                        if (!buildLineBuffer.empty() && buildLineBuffer.back() == '\r')
                            buildLineBuffer.pop_back();
                        if (!buildLineBuffer.empty())
                            primaryConsole->AddLog(ConsoleView::Level::Build, buildLineBuffer);
                        buildLineBuffer.clear();
                    }
                    else
                    {
                        buildLineBuffer += tmp[i];
                    }
                }
            }
        }

        // Poll the background cmake build process.
        if (playState == PlayState::Building && hBuildProcess)
        {
            if (WaitForSingleObject(hBuildProcess, 0) != WAIT_TIMEOUT)
            {
                DWORD exitCode = 1;
                GetExitCodeProcess(hBuildProcess, &exitCode);
                CloseHandle(hBuildProcess);
                hBuildProcess = nullptr;

                // Flush any remaining partial line from the pipe.
                if (!buildLineBuffer.empty())
                {
                    primaryConsole->AddLog(ConsoleView::Level::Build, buildLineBuffer);
                    buildLineBuffer.clear();
                }
                if (hBuildPipe) { CloseHandle(hBuildPipe); hBuildPipe = nullptr; }

                if (exitCode == 0)
                {
                    // Build succeeded — act on the requested post-build action.
                    switch (postBuildAction)
                    {
                        case PostBuildAction::PlayInEditor:
                            playState = PlayState::Playing;
                            for (const auto& obj : scene.GetObjects())
                                obj->Start();
                            primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded. Running in editor.");
                            break;
                        case PostBuildAction::LaunchStandalone:
                            LaunchGameExe();
                            playState = PlayState::Stopped;
                            primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded. Launching standalone.");
                            break;
                        case PostBuildAction::Nothing:
                        default:
                            playState = PlayState::Stopped;
                            primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded.");
                            break;
                    }
                    postBuildAction = PostBuildAction::Nothing;
                }
                else
                {
                    playState = PlayState::BuildFailed;
                    primaryConsole->AddLog(ConsoleView::Level::Error, "[Build] Build FAILED.");
                }
            }
        }

        // Tick game objects while the simulation is running.
        if (playState == PlayState::Playing)
        {
            for (const auto& obj : scene.GetObjects())
                obj->Update();
            hasUnsavedChanges = true;  // Mark as unsaved if game is running
        }

        renderer->MarkDirty();

        // Update window title to reflect unsaved-changes state.
        SetWindowTextW(window->GetHWND(),
                       hasUnsavedChanges ? L"Engine Editor *" : L"Engine Editor");

        renderer->RenderIfNeeded([&]()
        {
            renderer->Clear(projectSettings.clearColor.r, projectSettings.clearColor.g, projectSettings.clearColor.b);

            // Render every 3-D panel's offscreen texture before drawing ImGui.
            for (auto& panel : panels)
            {
                if (!panel->NeedsRender() || !panel->IsOpen()) continue;
                View* view = static_cast<View*>(panel.get());
                view->Render(renderer->GetCommandList(), renderer->GetCurrentRTV(),
                    [view](ID3D12GraphicsCommandList* cmd) { view->Render3D(cmd); });
            }

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

                ImGuiID centerTop, centerBottom;
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, &centerBottom, &centerTop);

                ImGui::DockBuilderDockWindow("Hierarchy 1",  left);
                ImGui::DockBuilderDockWindow("Assets 1",     left);
                ImGui::DockBuilderDockWindow("Scene 1",      centerTop);
                ImGui::DockBuilderDockWindow("Game 1",       centerTop);
                ImGui::DockBuilderDockWindow("Console 1",    centerBottom);
                ImGui::DockBuilderFinish(dockspaceId);
            }

            // ---------------------------------------------------------------
            // Main menu bar — File menu on the left, play controls in center.
            // ---------------------------------------------------------------
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Save All", "Ctrl+S"))
                    {
                        // Save scene changes
                        scene.Save(std::string(ASSETS_PATH) + "Scenes/default.scene");
                        // Save project settings changes
                        preferencesView.SaveSettings();
                        hasUnsavedChanges = false;
                    }
                    ImGui::Separator();
                    // Build actions
                    bool isBusy = (playState == PlayState::Building || playState == PlayState::Playing || playState == PlayState::Paused);
                    if (isBusy) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Build", "Ctrl+B"))
                    {
                        primaryConsole->Clear();
                        primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Starting build...");
                        postBuildAction = PostBuildAction::Nothing;
                        hBuildProcess   = StartGameBuild(hBuildPipe);
                        playState       = hBuildProcess ? PlayState::Building : PlayState::BuildFailed;
                    }
                    if (ImGui::MenuItem("Build and Run in Editor", nullptr))
                    {
                        primaryConsole->Clear();
                        primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Starting build (run in editor)...");
                        postBuildAction = PostBuildAction::PlayInEditor;
                        hBuildProcess   = StartGameBuild(hBuildPipe);
                        playState       = hBuildProcess ? PlayState::Building : PlayState::BuildFailed;
                    }
                    if (ImGui::MenuItem("Build and Run Standalone", nullptr))
                    {
                        primaryConsole->Clear();
                        primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Starting build (standalone)...");
                        postBuildAction = PostBuildAction::LaunchStandalone;
                        hBuildProcess   = StartGameBuild(hBuildPipe);
                        playState       = hBuildProcess ? PlayState::Building : PlayState::BuildFailed;
                    }
                    if (isBusy) ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Project Preferences"))
                        showPreferences = true;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
                    ImGui::EndMenu();
                }

                // Views menu — each item creates a new panel instance.
                if (ImGui::BeginMenu("Views"))
                {
                    bool no3D = !viewFactory.CanCreate3DView();
                    if (no3D) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Scene"))
                    {
                        auto p = viewFactory.Create("Scene");
                        if (p) panels.push_back(std::move(p));
                    }
                    if (ImGui::MenuItem("Game"))
                    {
                        auto p = viewFactory.Create("Game");
                        if (p) panels.push_back(std::move(p));
                    }
                    if (no3D) ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Hierarchy"))
                    {
                        auto p = viewFactory.Create("Hierarchy");
                        if (p) panels.push_back(std::move(p));
                    }
                    if (ImGui::MenuItem("Properties"))
                    {
                        auto p = viewFactory.Create("Properties");
                        if (p) panels.push_back(std::move(p));
                    }
                    if (ImGui::MenuItem("Assets"))
                    {
                        auto p = viewFactory.Create("Assets");
                        if (p) panels.push_back(std::move(p));
                    }
                    if (ImGui::MenuItem("Console"))
                    {
                        auto p = viewFactory.Create("Console");
                        if (p) panels.push_back(std::move(p));
                    }
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
                        primaryConsole->Clear();
                        primaryConsole->AddLog(ConsoleView::Level::Info, "[Build] Starting build (run in editor)...");
                        postBuildAction = PostBuildAction::PlayInEditor;
                        hBuildProcess   = StartGameBuild(hBuildPipe);
                        playState       = hBuildProcess ? PlayState::Building
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
                        if (hBuildPipe) { CloseHandle(hBuildPipe); hBuildPipe = nullptr; }
                        buildLineBuffer.clear();
                        primaryConsole->AddLog(ConsoleView::Level::Warning, "[Build] Build cancelled.");
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

            // Unsaved changes warning modal
            if (showUnsavedWarning)
            {
                ImGui::OpenPopup("Unsaved Changes");
            }

            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

            if (ImGui::BeginPopupModal("Unsaved Changes", NULL, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("You have unsaved changes. What would you like to do?");
                ImGui::Separator();

                if (ImGui::Button("Save & Load", ImVec2(150, 0)))
                {
                    scene.Save(std::string(ASSETS_PATH) + "Scenes/default.scene");
                    preferencesView.SaveSettings();
                    hasUnsavedChanges = false;
                    if (!sceneToLoad.empty())
                    {
                        scene.Load(sceneToLoad);
                        sceneToLoad.clear();
                    }
                    showUnsavedWarning = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();
                if (ImGui::Button("Load Without Saving", ImVec2(150, 0)))
                {
                    if (!sceneToLoad.empty())
                    {
                        scene.Load(sceneToLoad);
                        sceneToLoad.clear();
                    }
                    hasUnsavedChanges = false;
                    showUnsavedWarning = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0)))
                {
                    sceneToLoad.clear();
                    showUnsavedWarning = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            // Draw all open panels.
            for (auto& panel : panels)
                panel->DrawPanel();

            // Draw preferences window if open
            preferencesView.SetOpen(showPreferences);
            if (showPreferences)
            {
                preferencesView.DrawWindow(showPreferences);
            }

            // Remove panels that were closed this frame, returning SRV slots.
            for (auto it = panels.begin(); it != panels.end(); )
            {
                if (!(*it)->IsOpen())
                {
                    if ((*it)->NeedsRender())
                        viewFactory.FreeSrvSlot(static_cast<View*>(it->get())->GetSrvSlotIndex());
                    viewFactory.NotifyPanelRemoved(it->get());
                    it = panels.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        });
    };

    int result = window->Run();

    // Ensure the build process is cleaned up if the window is closed mid-build.
    if (hBuildProcess)
    {
        TerminateProcess(hBuildProcess, 1);
        CloseHandle(hBuildProcess);
    }
    if (hBuildPipe)
        CloseHandle(hBuildPipe);

    return result;
}
