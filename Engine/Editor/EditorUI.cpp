#include "pch.h"
#include "Engine/Editor/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "imgui_internal.h"

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

    ImGui::EndMainMenuBar();
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
        panel->DrawPanel();

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
    prefs->SetOpen(show);
    if (show)
        prefs->DrawWindow(show);
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
