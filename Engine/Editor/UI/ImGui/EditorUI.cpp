#include "pch.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "imgui_internal.h"
#include <algorithm>

// Fallback for IntelliSense
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// EditorUI::EditorUI
// ---------------------------------------------------------------------------
EditorUI::EditorUI(EditorState* state)
    : m_state(state)
{
    OutputDebugStringA("[EditorUI] Constructor called\n");
}

// ---------------------------------------------------------------------------
// EditorUI::Render
// ---------------------------------------------------------------------------
void EditorUI::Render(PlayState playState)
{
    if (!m_state)
    {
        return;
    }

    SetupDockingLayout();
    
    DrawMenuBar(playState);
    
    DrawPanels();
    CaptureDockingLayout();
    
    DrawPreferences();
    
    DrawUnsavedChangesModal();
}

// ---------------------------------------------------------------------------
// EditorUI::SetupDockingLayout
// ---------------------------------------------------------------------------
void EditorUI::SetupDockingLayout()
{
    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Build the default layout once — only when no saved layout exists.
    if (!m_dockingInitialized)
    {
        EditorLayout& layout = m_state->GetEditorLayout();
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID left, center, right;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left,
            layout.LeftWidthRatio(), &left, &center);
        const float remainingWidth = std::max(0.01f, 1.f - layout.LeftWidthRatio());
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right,
            std::clamp(layout.RightWidthRatio() / remainingWidth, 0.10f, 0.80f), &right, &center);

        ImGuiID centerTop, centerBottom;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,
            layout.ConsoleHeightRatio(), &centerBottom, &centerTop);

        const bool hierarchyActive = layout.ActiveLeftTab() == "Hierarchy 1";
        ImGui::DockBuilderDockWindow(hierarchyActive ? "Assets 1" : "Hierarchy 1", left);
        ImGui::DockBuilderDockWindow(hierarchyActive ? "Hierarchy 1" : "Assets 1", left);
        const bool sceneActive = layout.ActiveCenterTab() == "Scene 1";
        ImGui::DockBuilderDockWindow(sceneActive ? "Game 1" : "Scene 1", centerTop);
        ImGui::DockBuilderDockWindow(sceneActive ? "Scene 1" : "Game 1", centerTop);
        ImGui::DockBuilderDockWindow("Console 1",    centerBottom);
        ImGui::DockBuilderFinish(dockspaceId);
        m_leftDockId = left;
        m_rightDockId = right;
        m_centerTopDockId = centerTop;
        m_centerBottomDockId = centerBottom;
        m_dockingInitialized = true;
    }
}

void EditorUI::CaptureDockingLayout()
{
    if (!m_dockingInitialized) return;
    const ImVec2 viewportSize = ImGui::GetMainViewport()->Size;
    if (viewportSize.x <= 0.f || viewportSize.y <= 0.f) return;
    ImGuiDockNode* left = ImGui::DockBuilderGetNode(m_leftDockId);
    ImGuiDockNode* right = ImGui::DockBuilderGetNode(m_rightDockId);
    ImGuiDockNode* bottom = ImGui::DockBuilderGetNode(m_centerBottomDockId);
    if (left && right && bottom)
        m_state->GetEditorLayout().SetDockRatios(
            left->Size.x / viewportSize.x,
            right->Size.x / viewportSize.x,
            bottom->Size.y / viewportSize.y);

    auto captureActiveTab = [&](uint32_t dockId, bool leftGroup)
    {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (!node || !node->VisibleWindow) return;
        const std::string title = node->VisibleWindow->Name;
        if (leftGroup) m_state->GetEditorLayout().SetActiveLeftTab(title);
        else m_state->GetEditorLayout().SetActiveCenterTab(title);
    };
    captureActiveTab(m_leftDockId, true);
    captureActiveTab(m_centerTopDockId, false);
}

// ---------------------------------------------------------------------------
// EditorUI::DrawMenuBar
// ---------------------------------------------------------------------------
void EditorUI::DrawMenuBar(PlayState playState)
{
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Save All", "Ctrl+S"))
            OnMenuSave();
        ImGui::Separator();

        bool isBusy = (playState == PlayState::Building || 
                       playState == PlayState::Playing || 
                       playState == PlayState::Paused);
        if (isBusy) ImGui::BeginDisabled();

        if (ImGui::MenuItem("Build", "Ctrl+B"))
            OnMenuBuild();
        if (ImGui::MenuItem("Build and Run in Editor", nullptr))
            OnMenuBuildAndPlay();
        if (ImGui::MenuItem("Build and Run Standalone", nullptr))
            OnMenuBuildAndLaunchStandalone();

        if (isBusy) ImGui::EndDisabled();
        ImGui::Separator();

        if (ImGui::MenuItem("Project Preferences"))
            OnMenuPreferences();
        ImGui::Separator();

        if (ImGui::MenuItem("Exit"))
            OnMenuExit();

        ImGui::EndMenu();
    }

    // Views menu
    if (ImGui::BeginMenu("Views"))
    {
        ViewFactory* factory = m_state->GetViewFactory();
        bool no3D = !factory->CanCreate3DView();
        if (no3D) ImGui::BeginDisabled();

        if (ImGui::MenuItem("Scene"))
        {
            auto p = factory->Create("Scene");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }
        if (ImGui::MenuItem("Game"))
        {
            auto p = factory->Create("Game");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }

        if (no3D) ImGui::EndDisabled();
        ImGui::Separator();

        if (ImGui::MenuItem("Hierarchy"))
        {
            auto p = factory->Create("Hierarchy");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }
        if (ImGui::MenuItem("Properties"))
        {
            auto p = factory->Create("Properties");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }
        if (ImGui::MenuItem("Assets"))
        {
            auto p = factory->Create("Assets");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }
        if (ImGui::MenuItem("Console"))
        {
            auto p = factory->Create("Console");
            if (p) m_state->GetPanels().push_back(std::move(p));
        }

        ImGui::EndMenu();
    }

    DrawPlayControls(playState);

    ImGui::EndMainMenuBar(); // Balances BeginMainMenuBar above.
}

// ---------------------------------------------------------------------------
// EditorUI::DrawPlayControls
// ---------------------------------------------------------------------------
void EditorUI::DrawPlayControls(PlayState playState)
{
    const float singleBtnW = 60.f;
    const float doubleBtnW = singleBtnW * 2.f + 4.f;
    const float barW = ImGui::GetWindowWidth();

    if (playState == PlayState::Stopped || playState == PlayState::BuildFailed)
    {
        ImGui::SetCursorPosX((barW - singleBtnW) * 0.5f);
        if (ImGui::Button("Play", { singleBtnW, 0.f }))
            OnMenuBuildAndPlay();
        if (playState == PlayState::BuildFailed)
        {
            ImGui::SameLine();
            ImGui::TextColored({ 1.f, 0.3f, 0.3f, 1.f }, "Build failed");
        }
    }
    else if (playState == PlayState::Building)
    {
        static const char* kSpinner[] = { "|", "/", "-", "\\" };
        int frame = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
        ImGui::SetCursorPosX((barW - 120.f) * 0.5f);
        ImGui::Text("Building... %s", kSpinner[frame]);
    }
    else if (playState == PlayState::Playing)
    {
        ImGui::SetCursorPosX((barW - doubleBtnW) * 0.5f);
        if (ImGui::Button("Pause", { singleBtnW, 0.f }))
        {
            if (m_gameBuildManager) m_gameBuildManager->Pause();
        }
        ImGui::SameLine(0.f, 4.f);
        if (ImGui::Button("Stop", { singleBtnW, 0.f }))
        {
            if (m_gameBuildManager) m_gameBuildManager->Stop();
        }
    }
    else if (playState == PlayState::Paused)
    {
        ImGui::SetCursorPosX((barW - doubleBtnW) * 0.5f);
        if (ImGui::Button("Resume", { singleBtnW, 0.f }))
        {
            if (m_gameBuildManager) m_gameBuildManager->Resume();
        }
        ImGui::SameLine(0.f, 4.f);
        if (ImGui::Button("Stop", { singleBtnW, 0.f }))
        {
            if (m_gameBuildManager) m_gameBuildManager->Stop();
        }
    }
}

// ---------------------------------------------------------------------------
// EditorUI::DrawPanels
// ---------------------------------------------------------------------------
void EditorUI::DrawPanels()
{
    auto& panels = m_state->GetPanels();
    for (auto& panel : panels)
    {
        if (!panel) continue;
        if (panel->IsOpen()) panel->DrawPanel(m_ui);
        m_state->GetEditorLayout().SetPanelOpen(panel->GetTitle(), panel->IsOpen());
    }

    // Remove closed panels
    ViewFactory* factory = m_state->GetViewFactory();
    for (auto it = panels.begin(); it != panels.end(); )
    {
        if (!(*it)->IsOpen())
        {
            if ((*it)->NeedsRender())
            {
                // Safe cast to View to free SRV slot
                View* view = dynamic_cast<View*>(it->get());
                if (view)
                {
                    factory->FreeSrvSlot(view->GetSrvSlotIndex());
                }
            }
            factory->NotifyPanelRemoved(it->get());
            it = panels.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// EditorUI::DrawPreferences
// ---------------------------------------------------------------------------
void EditorUI::DrawPreferences()
{
    PreferencesView* prefs = m_state->GetPreferences();
    if (!prefs)
        return;

    bool show = m_state->IsShowingPreferences();
    if (show)
    {
        prefs->DrawWindow(m_ui, show);
        m_state->SetShowPreferences(show);
    }
    prefs->SetOpen(show);
}

// ---------------------------------------------------------------------------
// EditorUI::DrawUnsavedChangesModal
// ---------------------------------------------------------------------------
void EditorUI::DrawUnsavedChangesModal()
{
    if (!m_showUnsavedWarning)
        return;

    ImGui::OpenPopup("Unsaved Changes");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Unsaved Changes", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("You have unsaved changes. What would you like to do?");
        ImGui::Separator();

        if (ImGui::Button("Save & Load", ImVec2(150, 0)))
        {
            m_state->SaveScene();
            if (!m_sceneToLoadOnConfirm.empty())
                m_state->LoadScene(m_sceneToLoadOnConfirm);
            m_showUnsavedWarning = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Load Without Saving", ImVec2(150, 0)))
        {
            if (!m_sceneToLoadOnConfirm.empty())
                m_state->LoadScene(m_sceneToLoadOnConfirm);
            m_state->SetHasUnsavedChanges(false);
            m_showUnsavedWarning = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
        {
            m_sceneToLoadOnConfirm.clear();
            m_showUnsavedWarning = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuSave
// ---------------------------------------------------------------------------
void EditorUI::OnMenuSave()
{
    m_state->SaveScene();
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuBuild
// ---------------------------------------------------------------------------
void EditorUI::OnMenuBuild()
{
    if (m_gameBuildManager)
        m_gameBuildManager->StartBuild(PostBuildAction::Nothing);
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuBuildAndPlay
// ---------------------------------------------------------------------------
void EditorUI::OnMenuBuildAndPlay()
{
    if (m_gameBuildManager)
        m_gameBuildManager->StartBuild(PostBuildAction::PlayInEditor);
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuBuildAndLaunchStandalone
// ---------------------------------------------------------------------------
void EditorUI::OnMenuBuildAndLaunchStandalone()
{
    if (m_gameBuildManager)
        m_gameBuildManager->StartBuild(PostBuildAction::LaunchStandalone);
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuPreferences
// ---------------------------------------------------------------------------
void EditorUI::OnMenuPreferences()
{
    m_state->SetShowPreferences(true);
}

// ---------------------------------------------------------------------------
// EditorUI::OnMenuExit
// ---------------------------------------------------------------------------
void EditorUI::OnMenuExit()
{
    PostQuitMessage(0);
}
