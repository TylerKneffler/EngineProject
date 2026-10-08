#include "pch.h"
#include "Engine/Editor/UI/ImGui/Panels/ImGuiPanelHost.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "Engine/Editor/Core/View/View.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Engine/Editor/Core/View/Views/PreferencesView.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/AssetsExplorerView.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Engine::Editor
{
namespace
{
void PushCompactPanelStyle(bool tree)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(style.ItemSpacing.x, 3.f));
    if (tree)
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 13.f);
}

void PopCompactPanelStyle(bool tree)
{
    ImGui::PopStyleVar(tree ? 3 : 2);
}
}

void ImGuiPanelHost::DrawToolbar(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##EditorToolbar",
        ImGui::GetMainViewport(), ImGuiDir_Up,
        state.GetGlobalToolbarHeight(), flags))
    {
        if (state.GetToolbarVisibility().playControls)
        {
            const bool canPlay = buildManager &&
                (playState == PlayState::Stopped ||
                    playState == PlayState::BuildFailed ||
                    playState == PlayState::Paused);
            ImGui::BeginDisabled(!canPlay);
            if (ImGui::SmallButton("Play##ToolbarPlay"))
            {
                if (playState == PlayState::Paused) buildManager->Resume();
                else buildManager->PlayInEditor();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", playState == PlayState::Paused
                    ? "Resume" : playState == PlayState::Building
                    ? "Building project" : playState == PlayState::BuildFailed
                    ? "Last build failed; play to retry" : "Play in editor");
            ImGui::SameLine();
            ImGui::BeginDisabled(!buildManager ||
                playState != PlayState::Playing);
            if (ImGui::SmallButton("Pause##ToolbarPause"))
                buildManager->Pause();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!buildManager ||
                (playState != PlayState::Playing &&
                    playState != PlayState::Paused));
            if (ImGui::SmallButton("Stop##ToolbarStop"))
                buildManager->Stop();
            ImGui::EndDisabled();
            if (ImGui::GetContentRegionAvail().x >= 100.f)
                ImGui::SameLine();
        }
        state.DrawGlobalToolbar(m_ui);
        if (state.HasToolbarToolRow())
        {
            ImGui::Separator();
            state.DrawToolbarTools(m_ui);
        }
        ImGui::End();
    }
}

void ImGuiPanelHost::Draw(EditorState& state)
{
    DrawPanels(state);
    DrawPreferences(state);
}

void ImGuiPanelHost::DrawPanels(EditorState& state)
{
    auto& panels = state.GetPanels();
    for (auto& panel : panels)
        if (panel)
        {
            const bool compactTree = dynamic_cast<HierarchyView*>(panel.get()) ||
                dynamic_cast<AssetsExplorerView*>(panel.get());
            if (compactTree)
                PushCompactPanelStyle(true);
            if (panel->ConsumeDefaultDockPending())
                m_ui.DockWindowToArea(panel->GetTitle().c_str(),
                    panel->GetDefaultDockArea());
            panel->DrawPanel(m_ui);
            if (compactTree)
                PopCompactPanelStyle(true);
        }

    // Asset callbacks may request panels to be added. Apply those requests only
    // after traversal, because push_back can invalidate this vector's iterators.
    state.ProcessPendingPrefabStageOpen();
    state.ProcessPendingSceneAssetDocumentOpens();
    state.ProcessPendingAssetDocumentOpens();

    // The prefab scene tab is its own document window. Shared hierarchy and
    // properties panels retarget based on focused document.
    state.HandlePrefabPanelClosures();
    state.HandleSceneAssetDocumentClosures();
    state.HandleAssetDocumentClosures();

    ViewFactory* factory = state.GetViewFactory();
    for (auto it = panels.begin(); it != panels.end();)
    {
        if (*it && (*it)->IsOpen())
        {
            ++it;
            continue;
        }

        if (*it && (*it)->NeedsRender())
            if (auto* view = dynamic_cast<View*>(it->get()); view && factory)
                factory->FreeSrvSlot(view->GetSrvSlotIndex());
        if (*it && factory)
            factory->NotifyPanelRemoved(it->get());
        it = panels.erase(it);
    }
}

void ImGuiPanelHost::DrawPreferences(EditorState& state)
{
    PreferencesView* preferences = state.GetPreferences();
    if (!preferences) return;

    bool show = state.IsShowingPreferences();
    if (show)
    {
        PushCompactPanelStyle(false);
        preferences->DrawWindow(m_ui, show);
        PopCompactPanelStyle(false);
        state.SetShowPreferences(show);
    }
    preferences->SetOpen(show);
}
}
