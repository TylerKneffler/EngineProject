#include "pch.h"
#include "Engine/Editor/UI/ImGui/Panels/ImGuiPanelHost.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "Engine/Editor/Core/View/View.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Engine/Editor/Core/View/Views/PreferencesView.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Views/SceneView.h"
#include "Engine/Editor/Core/View/Views/AssetsExplorerView.h"
#include "Core/Component.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Engine::Editor
{
namespace
{
constexpr ImGuiWindowFlags ToolbarWindowFlags = ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

enum class TransportIcon { Play, Pause, Stop };

bool TransportButton(const char* id, TransportIcon icon)
{
    const bool clicked = ImGui::Button(id, ImVec2(28.f, 20.f));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * .5f, (min.y + max.y) * .5f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    switch (icon)
    {
    case TransportIcon::Play:
        drawList->AddTriangleFilled(
            ImVec2(center.x - 4.f, center.y - 5.f),
            ImVec2(center.x - 4.f, center.y + 5.f),
            ImVec2(center.x + 5.f, center.y), color);
        break;
    case TransportIcon::Pause:
        drawList->AddRectFilled(
            ImVec2(center.x - 5.f, center.y - 5.f),
            ImVec2(center.x - 2.f, center.y + 5.f), color);
        drawList->AddRectFilled(
            ImVec2(center.x + 2.f, center.y - 5.f),
            ImVec2(center.x + 5.f, center.y + 5.f), color);
        break;
    case TransportIcon::Stop:
        drawList->AddRectFilled(
            ImVec2(center.x - 5.f, center.y - 5.f),
            ImVec2(center.x + 5.f, center.y + 5.f), color);
        break;
    }
    return clicked;
}

struct ToolbarLayout
{
    float widths[8]{ 105.f, 90.f, 200.f, 180.f, 220.f, 180.f, 165.f, 240.f };
    bool sized[8]{};
    float singleSectionFill[2]{ 1.f, 1.f };
    int order[8]{ 0, 1, 2, 3, 7, 4, 5, 6 };
    int side[8]{ 0, 0, 0, 0, 1, 1, 1, 1 };
};

ToolbarLayout g_toolbarLayout;

void* ToolbarSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
    const char* name)
{
    return std::strcmp(name, "Main") == 0 ? handler->UserData : nullptr;
}

void ToolbarSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry,
    const char* line)
{
    auto& layout = *static_cast<ToolbarLayout*>(entry);
    int order[8]{};
    if (sscanf_s(line, "Order=%d,%d,%d,%d,%d,%d,%d,%d", &order[0], &order[1],
        &order[2], &order[3], &order[4], &order[5], &order[6], &order[7]) == 8)
    {
        bool seen[8]{};
        bool valid = true;
        for (int index : order)
            if (index < 0 || index >= 8 || seen[index]) valid = false;
            else seen[index] = true;
        if (valid)
            for (int i = 0; i < 8; ++i) layout.order[i] = order[i];
        return;
    }
    if (sscanf_s(line, "Order=%d,%d,%d,%d,%d,%d,%d", &order[0], &order[1],
        &order[2], &order[3], &order[4], &order[5], &order[6]) == 7)
    {
        bool seen[7]{};
        bool valid = true;
        for (int i = 0; i < 7; ++i)
            if (order[i] < 0 || order[i] >= 7 || seen[order[i]]) valid = false;
            else seen[order[i]] = true;
        if (valid)
        {
            for (int i = 0; i < 7; ++i) layout.order[i] = order[i];
            layout.order[7] = 7;
        }
        return;
    }
    int side[8]{};
    if (sscanf_s(line, "Side=%d,%d,%d,%d,%d,%d,%d,%d", &side[0], &side[1],
        &side[2], &side[3], &side[4], &side[5], &side[6], &side[7]) == 8)
    {
        bool valid = true;
        for (int value : side) valid &= value == 0 || value == 1;
        if (valid)
            for (int i = 0; i < 8; ++i) layout.side[i] = side[i];
        return;
    }
    if (sscanf_s(line, "Side=%d,%d,%d,%d,%d,%d,%d", &side[0], &side[1],
        &side[2], &side[3], &side[4], &side[5], &side[6]) == 7)
    {
        for (int i = 0; i < 7; ++i)
            if (side[i] == 0 || side[i] == 1) layout.side[i] = side[i];
        return;
    }
    float widths[8]{};
    if (sscanf_s(line, "Widths=%f,%f,%f,%f,%f,%f,%f,%f", &widths[0],
        &widths[1], &widths[2], &widths[3], &widths[4], &widths[5],
        &widths[6], &widths[7]) == 8)
    {
        bool valid = true;
        for (float width : widths)
            valid &= std::isfinite(width) && width >= 32.f && width <= 10000.f;
        if (valid)
            for (int i = 0; i < 8; ++i) layout.widths[i] = widths[i];
        return;
    }
    if (sscanf_s(line, "Widths=%f,%f,%f,%f,%f,%f,%f", &widths[0],
        &widths[1], &widths[2], &widths[3], &widths[4], &widths[5],
        &widths[6]) == 7)
    {
        for (int i = 0; i < 7; ++i)
            if (std::isfinite(widths[i]) && widths[i] >= 32.f &&
                widths[i] <= 10000.f) layout.widths[i] = widths[i];
        return;
    }
    float fill[2]{};
    int sized[8]{};
    if (sscanf_s(line, "Sized=%d,%d,%d,%d,%d,%d,%d,%d", &sized[0],
        &sized[1], &sized[2], &sized[3], &sized[4], &sized[5],
        &sized[6], &sized[7]) == 8)
    {
        for (int i = 0; i < 8; ++i) layout.sized[i] = sized[i] == 1;
        return;
    }
    if (sscanf_s(line, "Fill=%f,%f", &fill[0], &fill[1]) == 2 &&
        std::isfinite(fill[0]) && std::isfinite(fill[1]) &&
        fill[0] >= .18f && fill[0] <= 1.f &&
        fill[1] >= .18f && fill[1] <= 1.f)
    {
        layout.singleSectionFill[0] = fill[0];
        layout.singleSectionFill[1] = fill[1];
    }
}

void ToolbarSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
    ImGuiTextBuffer* output)
{
    const auto& layout = *static_cast<ToolbarLayout*>(handler->UserData);
    output->appendf("[Toolbar][Main]\n"
        "Order=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Side=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Widths=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n"
        "Sized=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Fill=%.3f,%.3f\n\n",
        layout.order[0], layout.order[1], layout.order[2], layout.order[3],
        layout.order[4], layout.order[5], layout.order[6], layout.order[7],
        layout.side[0], layout.side[1], layout.side[2], layout.side[3],
        layout.side[4], layout.side[5], layout.side[6], layout.side[7],
        layout.widths[0], layout.widths[1], layout.widths[2],
        layout.widths[3], layout.widths[4], layout.widths[5],
        layout.widths[6], layout.widths[7],
        layout.sized[0], layout.sized[1], layout.sized[2], layout.sized[3],
        layout.sized[4], layout.sized[5], layout.sized[6], layout.sized[7],
        layout.singleSectionFill[0],
        layout.singleSectionFill[1]);
}

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

void ImGuiPanelHost::RegisterToolbarSettings()
{
    if (ImGui::FindSettingsHandler("Toolbar")) return;
    ImGuiSettingsHandler handler;
    handler.TypeName = "Toolbar";
    handler.TypeHash = ImHashStr("Toolbar");
    handler.UserData = &g_toolbarLayout;
    handler.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* settings)
    { *static_cast<ToolbarLayout*>(settings->UserData) = ToolbarLayout{}; };
    handler.ReadOpenFn = ToolbarSettingsReadOpen;
    handler.ReadLineFn = ToolbarSettingsReadLine;
    handler.WriteAllFn = ToolbarSettingsWriteAll;
    ImGui::AddSettingsHandler(&handler);
}

void ImGuiPanelHost::ReserveToolbar(EditorState& state)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 4.f));
    ImGui::BeginViewportSideBar("##EditorToolbar", ImGui::GetMainViewport(),
        ImGuiDir_Up, state.GetGlobalToolbarHeight(), ToolbarWindowFlags);
    ImGui::End();
    ImGui::PopStyleVar();
}

void ImGuiPanelHost::DrawToolbar(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 4.f));
    if (ImGui::Begin("##EditorToolbar", nullptr, ToolbarWindowFlags))
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 origin(cursor.x, cursor.y + 2.f);
        const float available = ImGui::GetContentRegionAvail().x;
        const ImGuiStyle& toolbarStyle = ImGui::GetStyle();
        const float playWidth = state.GetToolbarVisibility().playControls
            ? 56.f + toolbarStyle.ItemSpacing.x : 0.f;
        const float sideWidth = std::max(0.f, (available - playWidth) * .5f - 5.f);
        auto& sectionWidths = g_toolbarLayout.widths;
        auto& sectionSized = g_toolbarLayout.sized;
        auto& singleSectionFill = g_toolbarLayout.singleSectionFill;
        auto& sectionOrder = g_toolbarLayout.order;
        auto& sectionSide = g_toolbarLayout.side;
        static int draggingSection = -1;
        static int dragMode = 0; // 1: resize with Shift, 2: reorder
        static ImVec2 dragStart{};
        static const char* names[]{ "File", "Edit", "Mode", "Tools",
            "Transform", "View", "Prefab", "Paint" };
        static const EditorState::ToolbarSection sections[]{
            EditorState::ToolbarSection::File,
            EditorState::ToolbarSection::Edit,
            EditorState::ToolbarSection::Mode,
            EditorState::ToolbarSection::Tools,
            EditorState::ToolbarSection::Transform,
            EditorState::ToolbarSection::View,
            EditorState::ToolbarSection::Prefab,
            EditorState::ToolbarSection::Paint };
        struct SectionRect { int index; float x; float width; };
        SectionRect sectionRects[2][8]{};
        int rectCounts[2]{};
        const auto drawSide = [&](const int* indices, int count, float x,
            float sideSpan, int side)
        {
            if (!count) return;
            float minimums[8]{};
            float minimumTotal = 0.f;
            for (int i = 0; i < count; ++i)
            {
                minimums[i] = state.GetToolbarSectionMinimumWidth(
                    sections[indices[i]]);
                minimumTotal += minimums[i];
            }
            const float span = count == 1
                ? std::max(std::min(sideSpan, minimumTotal),
                    sideSpan * singleSectionFill[side]) : sideSpan;
            const bool fitsMinimums = span >= minimumTotal;
            const float extra = std::max(0.f, span - minimumTotal);
            float customExtra = 0.f;
            for (int i = 1; i < count; ++i)
                if (sectionSized[indices[i]])
                    customExtra += std::max(0.f,
                        sectionWidths[indices[i]] - minimums[i]);
            const float customScale = customExtra > extra && customExtra > 0.f
                ? extra / customExtra : 1.f;
            float allocated[8]{};
            float used = 0.f;
            for (int i = 1; i < count; ++i)
            {
                allocated[i] = fitsMinimums
                    ? minimums[i] + (sectionSized[indices[i]]
                        ? std::max(0.f, sectionWidths[indices[i]] - minimums[i]) *
                            customScale : 0.f)
                    : span * minimums[i] / minimumTotal;
                used += allocated[i];
            }
            allocated[0] = span - used;
            float cursor = x;
            for (int i = 0; i < count; ++i)
            {
                const int index = indices[i];
                const float width = allocated[i];
                if (width < 32.f) { cursor += width; continue; }
                sectionRects[side][rectCounts[side]++] = {index, cursor, width};
                ImGui::PushID(index);
                if (i)
                    ImGui::GetWindowDrawList()->AddLine(
                        ImVec2(cursor + 2.f, origin.y + 2.f),
                        ImVec2(cursor + 2.f, origin.y + 21.f),
                        ImGui::GetColorU32(ImGuiCol_Separator));
                ImGui::SetCursorScreenPos(ImVec2(cursor + 7.f, origin.y));
                const float contentWidth = std::max(1.f, width - 30.f);
                bool overflow = false;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.f, 1.f));
                if (ImGui::BeginChild("SectionContents", ImVec2(contentWidth, 23.f),
                    false, ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse))
                    state.DrawGlobalToolbar(m_ui, sections[index], true, &overflow);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                const ImVec2 gripMin(cursor + width - 22.f, origin.y + 1.f);
                ImGui::SetCursorScreenPos(gripMin);
                ImGui::InvisibleButton("SectionGrip", ImVec2(16.f, 11.f));
                const bool gripHovered = ImGui::IsItemHovered();
                const bool gripActive = ImGui::IsItemActive();
                if (ImGui::IsItemActivated())
                {
                    draggingSection = index;
                    dragMode = ImGui::GetIO().KeyShift ? 1 : 0;
                    dragStart = ImGui::GetIO().MousePos;
                }
                if (gripActive && draggingSection == index)
                {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    const float dx = mouse.x - dragStart.x;
                    const float dy = mouse.y - dragStart.y;
                    if (!dragMode)
                    {
                        if (std::abs(dx) > 4.f || std::abs(dy) > 4.f)
                            dragMode = 2;
                    }
                    if (dragMode == 1 && count == 1)
                    {
                        const float lower = std::clamp(
                            minimumTotal / std::max(1.f, sideWidth), .18f, 1.f);
                        const float fill = std::clamp(singleSectionFill[side] +
                                ImGui::GetIO().MouseDelta.x /
                                    std::max(1.f, sideWidth), lower, 1.f);
                        if (fill != singleSectionFill[side])
                        {
                            singleSectionFill[side] = fill;
                            ImGui::MarkIniSettingsDirty();
                        }
                    }
                    else if (dragMode == 1 && count > 1 && extra > 1.f)
                    {
                        const int resizedPosition = i == 0 ? 1 : i;
                        const int resizedIndex = indices[resizedPosition];
                        const float delta = ImGui::GetIO().MouseDelta.x *
                            (i == 0 ? -1.f : 1.f);
                        const float nextWidth = allocated[resizedPosition] + delta;
                        if (nextWidth >= minimums[resizedPosition] &&
                            allocated[0] - delta >= minimums[0])
                        {
                            sectionWidths[resizedIndex] = nextWidth;
                            sectionSized[resizedIndex] = true;
                            ImGui::MarkIniSettingsDirty();
                        }
                    }
                }
                if (gripHovered || gripActive)
                {
                    ImGui::SetMouseCursor(dragMode == 1
                        ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
                    if (gripHovered && !gripActive)
                        ImGui::SetTooltip("%s: drag to move; Shift+drag to resize",
                            names[index]);
                }
                const ImU32 dotColor = ImGui::GetColorU32(gripHovered || gripActive
                    ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 2; ++column)
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(gripMin.x + 5.f + column * 6.f,
                                gripMin.y + 2.f + row * 3.f), 1.f, dotColor);
                if (overflow)
                {
                    ImGui::SetCursorScreenPos(ImVec2(cursor + width - 22.f,
                        origin.y + 13.f));
                    if (ImGui::InvisibleButton("SectionToolsButton",
                        ImVec2(16.f, 9.f)))
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
                        ImVec2(minimum.x + 4.f, minimum.y + 3.f),
                        ImVec2(minimum.x + 12.f, minimum.y + 3.f),
                        ImVec2(minimum.x + 8.f, minimum.y + 6.f),
                        ImGui::GetColorU32(ImGuiCol_Text));
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
        int left[8]{};
        int right[8]{};
        int leftCount = 0;
        int rightCount = 0;
        for (int position = 0; position < 8; ++position)
        {
            const int index = sectionOrder[position];
            if (state.HasToolbarSection(sections[index]))
            {
                if (sectionSide[index] == 0) left[leftCount++] = index;
                else right[rightCount++] = index;
            }
        }
        const auto drawFittingSide = [&](const int* indices, int count,
            float x, int side)
        {
            constexpr float overflowWidth = 30.f;
            const auto fittingCount = [&](float width)
            {
                float used = 0.f;
                int visible = 0;
                for (; visible < count; ++visible)
                {
                    const float minimum = state.GetToolbarSectionMinimumWidth(
                        sections[indices[visible]]);
                    if (used + minimum > width) break;
                    used += minimum;
                }
                return visible;
            };
            int visible = fittingCount(sideWidth);
            if (visible < count)
                visible = fittingCount(std::max(0.f, sideWidth - overflowWidth));
            const bool hasOverflow = visible < count;
            const float contentSpan = std::max(0.f,
                sideWidth - (hasOverflow ? overflowWidth : 0.f));
            drawSide(indices, visible, x, contentSpan, side);
            if (!hasOverflow) return;

            ImGui::PushID(side);
            ImGui::SetCursorScreenPos(ImVec2(x + sideWidth - 27.f, origin.y + 1.f));
            if (ImGui::Button("##HiddenToolbarSections", ImVec2(24.f, 20.f)))
                ImGui::OpenPopup("HiddenToolbarSections");
            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 6.f, min.y + 2.f),
                ImGui::GetColorU32(ImGuiCol_Text), "...");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("More toolbar tools");
            if (ImGui::BeginPopup("HiddenToolbarSections"))
            {
                for (int i = visible; i < count; ++i)
                {
                    const int index = indices[i];
                    ImGui::PushID(index);
                    if (i != visible) ImGui::Separator();
                    ImGui::TextUnformatted(names[index]);
                    state.DrawGlobalToolbar(m_ui, sections[index], false);
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        };
        drawFittingSide(left, leftCount, origin.x, 0);
        drawFittingSide(right, rightCount,
            origin.x + available - sideWidth, 1);
        if (draggingSection >= 0 && dragMode == 2)
        {
            const int targetSide = ImGui::GetIO().MousePos.x <
                origin.x + available * .5f ? 0 : 1;
            SectionRect candidates[8]{};
            int candidateCount = 0;
            for (int i = 0; i < rectCounts[targetSide]; ++i)
                if (sectionRects[targetSide][i].index != draggingSection)
                    candidates[candidateCount++] = sectionRects[targetSide][i];
            int slot = 0;
            while (slot < candidateCount && ImGui::GetIO().MousePos.x >=
                candidates[slot].x + candidates[slot].width * .5f)
                ++slot;
            const float markerX = slot < candidateCount
                ? candidates[slot].x : candidateCount
                ? candidates[candidateCount - 1].x +
                    candidates[candidateCount - 1].width
                : targetSide == 0 ? origin.x : origin.x + available - sideWidth;
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(markerX, origin.y), ImVec2(markerX, origin.y + 23.f),
                ImGui::GetColorU32(ImGuiCol_CheckMark), 2.f);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                const int before = slot < candidateCount
                    ? candidates[slot].index : -1;
                const int after = slot == candidateCount && candidateCount
                    ? candidates[candidateCount - 1].index : -1;
                int reordered[8]{};
                int output = 0;
                bool inserted = false;
                for (int position = 0; position < 8; ++position)
                {
                    const int index = sectionOrder[position];
                    if (index == draggingSection) continue;
                    if (index == before && !inserted)
                    {
                        reordered[output++] = draggingSection;
                        inserted = true;
                    }
                    reordered[output++] = index;
                    if (index == after && !inserted)
                    {
                        reordered[output++] = draggingSection;
                        inserted = true;
                    }
                }
                if (!inserted) reordered[output++] = draggingSection;
                for (int position = 0; position < 8; ++position)
                    sectionOrder[position] = reordered[position];
                sectionSide[draggingSection] = targetSide;
                ImGui::MarkIniSettingsDirty();
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            draggingSection = -1;
            dragMode = 0;
        }
        if (state.GetToolbarVisibility().playControls)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (available - playWidth) * .5f,
                origin.y + 1.f));
            const bool animationFocused = state.IsAnimationEditorFocused();
            const bool animationPlaying = state.IsAnimationPlaying();
            const bool canToggle = animationFocused
                ? state.CanToggleAnimationPlayback() : buildManager &&
                (playState == PlayState::Stopped ||
                    playState == PlayState::BuildFailed ||
                    playState == PlayState::Playing ||
                    playState == PlayState::Paused);
            ImGui::BeginDisabled(!canToggle);
            const TransportIcon toggleIcon = animationPlaying ||
                (!animationFocused && playState == PlayState::Playing)
                ? TransportIcon::Pause : TransportIcon::Play;
            if (TransportButton("##ToolbarPlayPause", toggleIcon))
            {
                if (animationFocused) state.ToggleAnimationPlayback();
                else if (playState == PlayState::Paused) buildManager->Resume();
                else if (playState == PlayState::Playing) buildManager->Pause();
                else buildManager->PlayInEditor();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", animationFocused
                    ? (animationPlaying ? "Pause animation" : "Play animation")
                    : playState == PlayState::Playing
                    ? "Pause" : playState == PlayState::Paused
                    ? "Resume" : playState == PlayState::Building
                    ? "Building project" : playState == PlayState::BuildFailed
                    ? "Last build failed; play to retry" : "Play in editor");
            ImGui::SameLine();
            ImGui::BeginDisabled(animationFocused
                ? !state.CanStopAnimationPlayback()
                : !buildManager || (playState != PlayState::Playing &&
                    playState != PlayState::Paused));
            if (TransportButton("##ToolbarStop", TransportIcon::Stop))
            {
                if (animationFocused) state.StopAnimationPlayback();
                else buildManager->Stop();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", animationFocused
                    ? "Stop animation" : "Stop");
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleVar();
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

    if (Engine::Core::Component::HasEditorReferencePick())
    {
        const bool confirmed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        ImGuiWindow* focused = ImGui::GetCurrentContext()->NavWindow;
        bool allowedFocus = !focused;
        for (const auto& panel : panels)
        {
            if (!panel || (!dynamic_cast<HierarchyView*>(panel.get()) &&
                !dynamic_cast<PropertiesView*>(panel.get()) &&
                !dynamic_cast<SceneView*>(panel.get())))
                continue;
            ImGuiWindow* window = ImGui::FindWindowByName(
                panel->GetTitle().c_str());
            if (window && focused &&
                window->RootWindow == focused->RootWindow)
            {
                allowedFocus = true;
                break;
            }
        }
        if (confirmed || !allowedFocus)
            Engine::Core::Component::CancelEditorReferencePick();
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
