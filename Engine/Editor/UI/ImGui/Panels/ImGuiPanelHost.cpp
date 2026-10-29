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
#include "Engine/Editor/Core/View/Views/PerformanceView.h"
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
    float widths[8]{ 126.f, 100.f, 150.f, 150.f, 176.f, 176.f, 100.f, 220.f };
    bool sized[8]{};
    // Keep Save at the left edge and reserve the center for Play/Stop.
    int order[8]{ 0, 1, 2, 4, 3, 7, 5, 6 };
    int side[8]{ 0, 0, 0, 1, 0, 1, 1, 1 };
    int floatMode[8]{ 0, 0, 0, 0, 0, 0, 0, 0 }; // left, center, right
};

ToolbarLayout g_toolbarLayout;

struct PanelVisibility
{
    bool performanceOpen = false;
    bool performanceSaved = false;
};

PanelVisibility g_panelVisibility;

void SavePanelVisibility(bool open)
{
    if (g_panelVisibility.performanceOpen == open &&
        g_panelVisibility.performanceSaved)
        return;
    g_panelVisibility.performanceOpen = open;
    g_panelVisibility.performanceSaved = true;
    ImGui::MarkIniSettingsDirty();
    // Debugger Stop can terminate the process before ImGui's periodic save.
    if (const char* path = ImGui::GetIO().IniFilename; path && path[0])
        ImGui::SaveIniSettingsToDisk(path);
}

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
    int floatMode[8]{};
    if (sscanf_s(line, "Float=%d,%d,%d,%d,%d,%d,%d,%d",
        &floatMode[0], &floatMode[1], &floatMode[2], &floatMode[3],
        &floatMode[4], &floatMode[5], &floatMode[6], &floatMode[7]) == 8)
    {
        bool valid = true;
        for (int value : floatMode) valid &= value >= 0 && value <= 2;
        if (valid)
            for (int i = 0; i < 8; ++i)
                layout.floatMode[i] = floatMode[i];
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
    int sized[8]{};
    if (sscanf_s(line, "Sized=%d,%d,%d,%d,%d,%d,%d,%d", &sized[0],
        &sized[1], &sized[2], &sized[3], &sized[4], &sized[5],
        &sized[6], &sized[7]) == 8)
    {
        for (int i = 0; i < 8; ++i) layout.sized[i] = sized[i] == 1;
        return;
    }
}

void ToolbarSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
    ImGuiTextBuffer* output)
{
    const auto& layout = *static_cast<ToolbarLayout*>(handler->UserData);
    output->appendf("[Toolbar][Main]\n"
        "Order=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Side=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Float=%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Widths=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n"
        "Sized=%d,%d,%d,%d,%d,%d,%d,%d\n\n",
        layout.order[0], layout.order[1], layout.order[2], layout.order[3],
        layout.order[4], layout.order[5], layout.order[6], layout.order[7],
        layout.side[0], layout.side[1], layout.side[2], layout.side[3],
        layout.side[4], layout.side[5], layout.side[6], layout.side[7],
        layout.floatMode[0], layout.floatMode[1], layout.floatMode[2],
        layout.floatMode[3], layout.floatMode[4], layout.floatMode[5],
        layout.floatMode[6], layout.floatMode[7],
        layout.widths[0], layout.widths[1], layout.widths[2],
        layout.widths[3], layout.widths[4], layout.widths[5],
        layout.widths[6], layout.widths[7],
        layout.sized[0], layout.sized[1], layout.sized[2], layout.sized[3],
        layout.sized[4], layout.sized[5], layout.sized[6], layout.sized[7]);
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

    if (!ImGui::FindSettingsHandler("EditorPanels"))
    {
        g_panelVisibility = {};
        ImGuiSettingsHandler panels;
        panels.TypeName = "EditorPanels";
        panels.TypeHash = ImHashStr(panels.TypeName);
        panels.UserData = &g_panelVisibility;
        panels.ReadInitFn = [](ImGuiContext*, ImGuiSettingsHandler* settings)
        { *static_cast<PanelVisibility*>(settings->UserData) = {}; };
        panels.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* settings,
            const char* name) -> void*
        { return std::strcmp(name, "Visibility") == 0 ? settings->UserData : nullptr; };
        panels.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry,
            const char* line)
        {
            int value = -1;
            if (sscanf_s(line, "Performance=%d", &value) == 1 &&
                (value == 0 || value == 1))
            {
                auto& visibility = *static_cast<PanelVisibility*>(entry);
                visibility.performanceOpen = value != 0;
                visibility.performanceSaved = true;
            }
        };
        panels.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* settings,
            ImGuiTextBuffer* output)
        {
            const auto& visibility = *static_cast<PanelVisibility*>(settings->UserData);
            output->appendf("[EditorPanels][Visibility]\nPerformance=%d\n\n",
                visibility.performanceOpen);
        };
        ImGui::AddSettingsHandler(&panels);
    }
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
        const float sideWidth = std::max(0.f,
            (available - playWidth) * .5f - 5.f);
        auto& sectionWidths = g_toolbarLayout.widths;
        auto& sectionSized = g_toolbarLayout.sized;
        auto& sectionOrder = g_toolbarLayout.order;
        auto& sectionSide = g_toolbarLayout.side;
        auto& sectionFloat = g_toolbarLayout.floatMode;
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
        int visualOrder[8]{};
        int visualCount = 0;
        for (int side = 0; side < 2; ++side)
            for (int alignment = 0; alignment < 3; ++alignment)
                for (int position = 0; position < 8; ++position)
                {
                    const int index = sectionOrder[position];
                    if (sectionSide[index] == side &&
                        sectionFloat[index] == alignment &&
                        state.HasToolbarSection(sections[index]))
                        visualOrder[visualCount++] = index;
                }
        const auto swapWithNeighbor = [&](int index, int direction)
        {
            int visualPosition = 0;
            while (visualPosition < visualCount &&
                visualOrder[visualPosition] != index) ++visualPosition;
            const int neighborPosition = visualPosition + direction;
            if (neighborPosition < 0 || neighborPosition >= visualCount)
                return false;
            const int neighbor = visualOrder[neighborPosition];
            int orderPosition = 0;
            int neighborOrderPosition = 0;
            for (int position = 0; position < 8; ++position)
            {
                if (sectionOrder[position] == index) orderPosition = position;
                if (sectionOrder[position] == neighbor)
                    neighborOrderPosition = position;
            }
            std::swap(sectionOrder[orderPosition],
                sectionOrder[neighborOrderPosition]);
            std::swap(sectionSide[index], sectionSide[neighbor]);
            std::swap(sectionFloat[index], sectionFloat[neighbor]);
            ImGui::MarkIniSettingsDirty();
            return true;
        };
        const auto moveToEdge = [&](int index, int side)
        {
            int reordered[8]{};
            int next = 0;
            if (side == 0) reordered[next++] = index;
            for (int position = 0; position < 8; ++position)
                if (sectionOrder[position] != index)
                    reordered[next++] = sectionOrder[position];
            if (side == 1) reordered[next++] = index;
            for (int position = 0; position < 8; ++position)
                sectionOrder[position] = reordered[position];
            sectionSide[index] = side;
            sectionFloat[index] = side == 0 ? 0 : 2;
            ImGui::MarkIniSettingsDirty();
        };
        const auto drawSide = [&](const int* indices, int count, float x,
            float sideSpan, int side)
        {
            if (!count) return 0.f;
            float minimums[8]{};
            float minimumTotal = 0.f;
            for (int i = 0; i < count; ++i)
            {
                minimums[i] = state.GetToolbarSectionMinimumWidth(
                    sections[indices[i]]);
                minimumTotal += minimums[i];
            }
            float allocated[8]{};
            float remaining = sideSpan;
            float remainingMinimum = minimumTotal;
            for (int i = 0; i < count; ++i)
            {
                remainingMinimum -= minimums[i];
                const int index = indices[i];
                const float preferred = std::max(minimums[i],
                    sectionSized[index] ? sectionWidths[index] :
                        state.GetToolbarSectionPreferredWidth(sections[index]));
                allocated[i] = sideSpan >= minimumTotal
                    ? std::min(preferred, remaining - remainingMinimum)
                    : sideSpan * minimums[i] / minimumTotal;
                remaining -= allocated[i];
            }
            float cursor = x;
            for (int i = 0; i < count; ++i)
            {
                const int index = indices[i];
                const float width = allocated[i];
                if (width < 32.f) { cursor += width; continue; }
                sectionRects[side][rectCounts[side]++] = {index, cursor, width};
                ImGui::PushID(index);
                ImVec4 sectionColor = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
                sectionColor.w *= .55f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(cursor + 5.f, origin.y - 1.f),
                    ImVec2(cursor + width - 3.f, origin.y + 25.f),
                    ImGui::GetColorU32(sectionColor), 5.f);
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
                const bool gripActivated = ImGui::IsItemActivated();
                ImGui::OpenPopupOnItemClick("SectionPlacementMenu",
                    ImGuiPopupFlags_MouseButtonRight);
                if (ImGui::BeginPopup("SectionPlacementMenu"))
                {
                    int visualPosition = 0;
                    while (visualPosition < visualCount &&
                        visualOrder[visualPosition] != index) ++visualPosition;
                    ImGui::BeginDisabled(visualPosition == 0 ||
                        visualPosition == visualCount);
                    if (ImGui::ArrowButton("##MoveSectionLeft", ImGuiDir_Left))
                    {
                        swapWithNeighbor(index, -1);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Swap with previous section");
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(visualPosition + 1 >= visualCount);
                    if (ImGui::ArrowButton("##MoveSectionRight", ImGuiDir_Right))
                    {
                        swapWithNeighbor(index, 1);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Swap with next section");
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Move to left edge"))
                        moveToEdge(index, 0);
                    if (ImGui::MenuItem("Move to right edge"))
                        moveToEdge(index, 1);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset toolbar layout"))
                    {
                        g_toolbarLayout = ToolbarLayout{};
                        ImGui::MarkIniSettingsDirty();
                    }
                    ImGui::EndPopup();
                }
                if (gripActivated)
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
                    if (dragMode == 1)
                    {
                        const float nextWidth = std::clamp(
                            allocated[i] + ImGui::GetIO().MouseDelta.x,
                            minimums[i], std::max(minimums[i], sideWidth));
                        if (nextWidth != sectionWidths[index] ||
                            !sectionSized[index])
                        {
                            sectionWidths[index] = nextWidth;
                            sectionSized[index] = true;
                            ImGui::MarkIniSettingsDirty();
                        }
                    }
                }
                if (gripHovered || gripActive)
                {
                    ImGui::SetMouseCursor(dragMode == 1
                        ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
                    if (gripHovered && !gripActive)
                        ImGui::SetTooltip("%s: drag to move; drag right divider to resize; right-click for options",
                            names[index]);
                }
                const ImU32 dotColor = ImGui::GetColorU32(gripHovered || gripActive
                    ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 2; ++column)
                        ImGui::GetWindowDrawList()->AddCircleFilled(
                            ImVec2(gripMin.x + 5.f + column * 6.f,
                                gripMin.y + 2.f + row * 3.f), 1.f, dotColor);
                ImGui::SetCursorScreenPos(ImVec2(cursor + width - 6.f,
                    origin.y + 1.f));
                ImGui::InvisibleButton("SectionResize", ImVec2(6.f, 22.f));
                const bool resizeHovered = ImGui::IsItemHovered();
                const bool resizeActive = ImGui::IsItemActive();
                if (resizeHovered || resizeActive)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    if (resizeHovered && !resizeActive)
                        ImGui::SetTooltip("Drag to resize %s", names[index]);
                }
                if (resizeActive && ImGui::GetIO().MouseDelta.x != 0.f)
                {
                    sectionWidths[index] = std::clamp(
                        allocated[i] + ImGui::GetIO().MouseDelta.x,
                        minimums[i], std::max(minimums[i], sideWidth));
                    sectionSized[index] = true;
                    ImGui::MarkIniSettingsDirty();
                }
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(cursor + width - 3.f, origin.y + 5.f),
                    ImVec2(cursor + width - 3.f, origin.y + 19.f),
                    ImGui::GetColorU32(resizeHovered || resizeActive
                        ? ImGuiCol_Text : ImGuiCol_Separator));
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
            return cursor - x;
        };
        int groups[2][3][8]{};
        int groupCounts[2][3]{};
        for (int position = 0; position < 8; ++position)
        {
            const int index = sectionOrder[position];
            if (state.HasToolbarSection(sections[index]))
                groups[sectionSide[index]][sectionFloat[index]]
                    [groupCounts[sectionSide[index]][sectionFloat[index]]++] = index;
        }
        const auto drawFittingSide = [&](const int* indices, int count,
            float x, float span, int side, int alignment)
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
            int visible = fittingCount(span);
            if (visible < count)
                visible = fittingCount(std::max(0.f, span - overflowWidth));
            const bool hasOverflow = visible < count;
            const float contentSpan = std::max(0.f,
                span - (hasOverflow ? overflowWidth : 0.f));
            const float usedWidth = drawSide(indices, visible, x,
                contentSpan, side);
            if (!hasOverflow) return;

            ImGui::PushID(side * 3 + alignment);
            const float overflowX = alignment == 0
                ? x + usedWidth + 3.f : x + span - 27.f;
            ImGui::SetCursorScreenPos(ImVec2(overflowX, origin.y + 1.f));
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
        for (int side = 0; side < 2; ++side)
        {
            const float regionX = side == 0 ? origin.x :
                origin.x + available - sideWidth;
            float desired[3]{};
            float minimums[3]{};
            float totalDesired = 0.f;
            float totalMinimum = 0.f;
            for (int alignment = 0; alignment < 3; ++alignment)
            {
                for (int i = 0; i < groupCounts[side][alignment]; ++i)
                {
                    const int index = groups[side][alignment][i];
                    const float minimum =
                        state.GetToolbarSectionMinimumWidth(sections[index]);
                    minimums[alignment] += minimum;
                    desired[alignment] += std::max(minimum,
                        sectionSized[index] ? sectionWidths[index] :
                            state.GetToolbarSectionPreferredWidth(sections[index]));
                }
                totalDesired += desired[alignment];
                totalMinimum += minimums[alignment];
            }
            float widths[3]{};
            if (totalDesired <= sideWidth)
                for (int alignment = 0; alignment < 3; ++alignment)
                    widths[alignment] = desired[alignment];
            else if (totalMinimum <= sideWidth)
            {
                const float extra = totalDesired - totalMinimum;
                const float scale = extra > 0.f
                    ? (sideWidth - totalMinimum) / extra : 0.f;
                for (int alignment = 0; alignment < 3; ++alignment)
                    widths[alignment] = minimums[alignment] +
                        (desired[alignment] - minimums[alignment]) * scale;
            }
            else if (totalMinimum > 0.f)
                for (int alignment = 0; alignment < 3; ++alignment)
                    widths[alignment] = sideWidth *
                        minimums[alignment] / totalMinimum;
            const float leftEnd = regionX + widths[0];
            const float rightStart = regionX + sideWidth - widths[2];
            const float middleX = std::clamp(
                regionX + (sideWidth - widths[1]) * .5f,
                leftEnd, std::max(leftEnd, rightStart - widths[1]));
            const float positions[3]{ regionX, middleX, rightStart };
            for (int alignment = 0; alignment < 3; ++alignment)
                if (groupCounts[side][alignment])
                    drawFittingSide(groups[side][alignment],
                        groupCounts[side][alignment], positions[alignment],
                        widths[alignment], side, alignment);
        }
        if (draggingSection >= 0 && dragMode == 2)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool overToolbar = mouse.x >= origin.x &&
                mouse.x <= origin.x + available &&
                mouse.y >= origin.y - 4.f && mouse.y <= origin.y + 27.f;
            if (overToolbar)
            {
            const int targetSide = mouse.x <
                origin.x + available * .5f ? 0 : 1;
            const float regionX = targetSide == 0 ? origin.x :
                origin.x + available - sideWidth;
            const float hoverPosition = sideWidth > 0.f
                ? std::clamp((mouse.x - regionX) / sideWidth, 0.f, 1.f)
                : .5f;
            const int targetFloat = hoverPosition < 1.f / 3.f ? 0 :
                hoverPosition < 2.f / 3.f ? 1 : 2;
            SectionRect candidates[8]{};
            int candidateCount = 0;
            for (int i = 0; i < rectCounts[targetSide]; ++i)
                if (sectionRects[targetSide][i].index != draggingSection &&
                    sectionFloat[sectionRects[targetSide][i].index] == targetFloat)
                    candidates[candidateCount++] = sectionRects[targetSide][i];
            int slot = 0;
            while (slot < candidateCount && mouse.x >=
                candidates[slot].x + candidates[slot].width * .5f)
                ++slot;
            const float markerX = slot < candidateCount
                ? candidates[slot].x : candidateCount
                ? candidates[candidateCount - 1].x +
                    candidates[candidateCount - 1].width
                : regionX +
                    (targetFloat == 0 ? 0.f : targetFloat == 1
                        ? sideWidth * .5f : sideWidth);
            ImDrawList* magnet = ImGui::GetForegroundDrawList();
            const ImU32 glow = ImGui::GetColorU32(
                ImVec4{.30f, .75f, 1.f, .25f});
            const ImU32 ink = ImGui::GetColorU32(ImGuiCol_CheckMark);
            magnet->AddLine(ImVec2(markerX, origin.y - 2.f),
                ImVec2(markerX, origin.y + 25.f), glow, 6.f);
            magnet->AddLine(ImVec2(markerX, origin.y - 2.f),
                ImVec2(markerX, origin.y + 25.f), ink, 2.f);
            magnet->AddLine(ImVec2(markerX - 4.f, origin.y - 2.f),
                ImVec2(markerX + 4.f, origin.y - 2.f), ink, 2.f);
            magnet->AddLine(ImVec2(markerX - 4.f, origin.y + 25.f),
                ImVec2(markerX + 4.f, origin.y + 25.f), ink, 2.f);
            ImGui::SetTooltip("Drop %s: float %s", names[draggingSection],
                targetFloat == 0 ? "left" : targetFloat == 1 ? "center" : "right");
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
                sectionFloat[draggingSection] = targetFloat;
                ImGui::MarkIniSettingsDirty();
            }
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            draggingSection = -1;
            dragMode = 0;
        }
        if (state.GetToolbarVisibility().playControls)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x +
                (available - playWidth) * .5f,
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
    state.UpdateAnimationPlayback(ImGui::GetIO().DeltaTime);
    auto& panels = state.GetPanels();
    bool hasPerformance = false;
    for (const auto& panel : panels)
        hasPerformance |= dynamic_cast<PerformanceView*>(panel.get()) != nullptr;

    // Older layouts saved the window position but had no panel visibility entry.
    if (!g_panelVisibility.performanceSaved &&
        ImGui::FindWindowSettingsByID(ImHashStr("Performance")))
        SavePanelVisibility(true);
    if (g_panelVisibility.performanceOpen && !hasPerformance)
        if (ViewFactory* factory = state.GetViewFactory())
            if (auto performance = factory->Create("Performance"))
                panels.push_back(std::move(performance));

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
            if (dynamic_cast<PerformanceView*>(panel.get()))
                SavePanelVisibility(panel->IsOpen());
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
    state.DrawAnimationCloseConfirmation(m_ui);
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
