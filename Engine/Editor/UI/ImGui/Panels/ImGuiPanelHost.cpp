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
#include <algorithm>

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
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float available = ImGui::GetContentRegionAvail().x;
        const float playWidth = state.GetToolbarVisibility().playControls
            ? 145.f : 0.f;
        const float sideWidth = std::max(0.f, (available - playWidth) * .5f - 5.f);
        static float sectionWidths[]{ 105.f, 90.f, 200.f, 180.f,
            220.f, 180.f, 165.f };
        static bool sectionOverflow[7]{};
        static const char* names[]{ "File", "Edit", "Mode", "Tools",
            "Transform", "View", "Prefab" };
        static const EditorState::ToolbarSection sections[]{
            EditorState::ToolbarSection::File,
            EditorState::ToolbarSection::Edit,
            EditorState::ToolbarSection::Mode,
            EditorState::ToolbarSection::Tools,
            EditorState::ToolbarSection::Transform,
            EditorState::ToolbarSection::View,
            EditorState::ToolbarSection::Prefab };
        const auto drawSide = [&](const int* indices, int count, float x)
        {
            if (!count) return;
            float total = 0.f;
            for (int i = 0; i < count; ++i)
                total += sectionWidths[indices[i]];
            float cursor = x;
            for (int i = 0; i < count; ++i)
            {
                const int index = indices[i];
                const float width = i == count - 1
                    ? x + sideWidth - cursor
                    : sideWidth * sectionWidths[index] / total;
                if (width < 33.f) { cursor += width; continue; }
                ImGui::PushID(index);
                if (i)
                    ImGui::GetWindowDrawList()->AddLine(
                        ImVec2(cursor + 2.f, origin.y + 2.f),
                        ImVec2(cursor + 2.f, origin.y + 21.f),
                        ImGui::GetColorU32(ImGuiCol_Separator));
                ImGui::SetCursorScreenPos(ImVec2(cursor + 7.f, origin.y));
                const bool reserveArrow = sectionOverflow[index];
                const float contentWidth = std::max(1.f,
                    width - (reserveArrow ? 34.f : 14.f));
                bool overflow = false;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.f, 1.f));
                if (ImGui::BeginChild("SectionContents", ImVec2(contentWidth, 23.f),
                    false, ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse))
                    state.DrawGlobalToolbar(m_ui, sections[index], true, &overflow);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                sectionOverflow[index] = overflow;
                if (reserveArrow && overflow)
                {
                    ImGui::SetCursorScreenPos(ImVec2(cursor + width - 23.f,
                        origin.y + 2.f));
                    const ImVec2 arrowSize(14.f, 18.f);
                    if (ImGui::InvisibleButton("SectionToolsButton", arrowSize))
                        ImGui::OpenPopup("SectionTools");
                    const ImVec2 minimum = ImGui::GetItemRectMin();
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::GetWindowDrawList()->AddRectFilled(minimum,
                            ImGui::GetItemRectMax(),
                            ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.f);
                        ImGui::SetTooltip("More %s tools", names[index]);
                    }
                    ImGui::GetWindowDrawList()->AddTriangleFilled(
                        ImVec2(minimum.x + 4.f, minimum.y + 7.f),
                        ImVec2(minimum.x + 10.f, minimum.y + 7.f),
                        ImVec2(minimum.x + 7.f, minimum.y + 10.f),
                        ImGui::GetColorU32(ImGuiCol_Text));
                }
                if (i + 1 < count)
                {
                    ImGui::SetCursorScreenPos(ImVec2(cursor + width - 6.f, origin.y));
                    ImGui::InvisibleButton("SectionResize", ImVec2(6.f, 23.f));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    if (ImGui::IsItemActive())
                    {
                        const float scale = total / std::max(1.f, sideWidth);
                        const float delta = ImGui::GetIO().MouseDelta.x * scale;
                        const int next = indices[i + 1];
                        if (sectionWidths[index] + delta >= 34.f &&
                            sectionWidths[next] - delta >= 34.f)
                        {
                            sectionWidths[index] += delta;
                            sectionWidths[next] -= delta;
                        }
                    }
                }
                if (ImGui::IsPopupOpen("SectionTools"))
                    ImGui::SetNextWindowSize(ImVec2(index == 3 ? 540.f :
                        index == 6 ? 480.f : 360.f, 0.f), ImGuiCond_Appearing);
                if (ImGui::BeginPopup("SectionTools"))
                {
                    ImGui::TextUnformatted(names[index]);
                    ImGui::Separator();
                    state.DrawGlobalToolbar(m_ui, sections[index], false);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                cursor += width;
            }
        };
        int left[4]{};
        int right[3]{};
        int leftCount = 0;
        int rightCount = 0;
        for (int i = 0; i < 7; ++i)
            if (state.HasToolbarSection(sections[i]))
            {
                if (i < 4) left[leftCount++] = i;
                else right[rightCount++] = i;
            }
        drawSide(left, leftCount, origin.x);
        drawSide(right, rightCount, origin.x + available - sideWidth);
        if (state.GetToolbarVisibility().playControls)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (available - playWidth) * .5f,
                origin.y));
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
        }
        ImGui::PopStyleVar();
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
    state.ProcessPendingAnimationDocumentOpens();
    state.ProcessPendingAssetDocumentOpens();

    // The prefab scene tab is its own document window. Shared hierarchy and
    // properties panels retarget based on focused document.
    state.HandlePrefabPanelClosures();
    state.HandleSceneAssetDocumentClosures();
    state.HandleAnimationDocumentClosures();
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
