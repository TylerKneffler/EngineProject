#include "pch.h"
#include "Engine/Editor/UI/ImGui/Layout/ImGuiDockspace.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace Engine::Editor
{
void ImGuiDockspace::Draw()
{
    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Build defaults only when imgui.ini did not restore a dock tree.
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (root != nullptr && !root->IsLeafNode())
    {
        if (!m_startupMigrationComplete)
        {
            // Seed names from older layouts once. Checking saved settings,
            // rather than the live DockNode, distinguishes a genuinely old
            // layout from a current window that the user just undocked.
            const auto migrateLegacyName = [](const char* currentName,
                const char* legacyName)
            {
                if (ImGui::FindWindowSettingsByID(ImHashStr(currentName)))
                    return;
                ImGuiWindowSettings* legacy = ImGui::FindWindowSettingsByID(
                    ImHashStr(legacyName));
                if (legacy && legacy->DockId != 0 &&
                    ImGui::DockBuilderGetNode(legacy->DockId))
                {
                    ImGui::DockBuilderDockWindow(currentName, legacy->DockId);
                }
            };
            migrateLegacyName("Scene", "Scene 1");
            migrateLegacyName("Game", "Game 1");
            migrateLegacyName("Hierarchy", "Hierarchy 1");
            migrateLegacyName("Properties", "Properties 1");
            migrateLegacyName("Assets", "Assets 1");
            migrateLegacyName("Console", "Console 1");
            migrateLegacyName("Problems", "Problems 1");
            migrateLegacyName("Terminal", "Terminal 1");
            m_startupMigrationComplete = true;
        }
        return;
    }

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspaceId,
        ImGui::GetMainViewport()->WorkPos);
    ImGui::DockBuilderSetNodeSize(dockspaceId,
        ImGui::GetMainViewport()->WorkSize);

    ImGuiID left, center, right;
    ImGui::DockBuilderSplitNode(
        dockspaceId, ImGuiDir_Left, 0.20f, &left, &center);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right, 0.31f, &right, &center);

    ImGuiID centerTop, centerBottom;
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, 0.30f, &centerBottom, &centerTop);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Scene", centerTop);
    ImGui::DockBuilderDockWindow("Game", centerTop);
    ImGui::DockBuilderDockWindow("Assets", centerBottom);
    ImGui::DockBuilderDockWindow("Console", centerBottom);
    ImGui::DockBuilderDockWindow("Problems", centerBottom);
    ImGui::DockBuilderDockWindow("Terminal", centerBottom);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderFinish(dockspaceId);
    m_startupMigrationComplete = true;
}

void ImGuiDockspace::DockWindowToArea(const char* title, EditorPanelDockArea area)
{
    if (!title || !title[0] || area == EditorPanelDockArea::None)
        return;

    // A default applies only to a new window. Restored floating windows and
    // custom dock positions belong to the user's saved layout.
    ImGuiWindowSettings* saved = ImGui::FindWindowSettingsByID(ImHashStr(title));
    if (saved && (saved->DockId == 0 || ImGui::DockBuilderGetNode(saved->DockId)))
        return;

    const auto nodeForWindow = [](const char* name) -> ImGuiID
    {
        ImGuiWindow* window = ImGui::FindWindowByName(name);
        if (window && window->DockNode)
            return window->DockNode->ID;
        // DockBuilder may have seeded the default before the anchor's first Begin.
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(name));
        return settings && settings->DockId && ImGui::DockBuilderGetNode(settings->DockId)
            ? settings->DockId : 0;
    };
    const auto nodeForViewType = [&](const char* baseName) -> ImGuiID
    {
        if (ImGuiID node = nodeForWindow(baseName)) return node;
        for (int slot = 2; slot <= 32; ++slot)
        {
            const std::string numbered = std::string(baseName) + " " + std::to_string(slot);
            if (ImGuiID node = nodeForWindow(numbered.c_str())) return node;
        }
        return 0;
    };

    ImGuiID targetNode = 0;
    switch (area)
    {
    case EditorPanelDockArea::MainDocument:
        targetNode = nodeForViewType("Scene");
        if (!targetNode) targetNode = nodeForViewType("Game");
        if (!targetNode)
        {
            // Document tabs may be the only windows left in the center.
            ImGuiContext& context = *ImGui::GetCurrentContext();
            for (int i = 0; i < context.DockContext.Nodes.Data.Size; ++i)
            {
                auto* node = static_cast<ImGuiDockNode*>(context.DockContext.Nodes.Data[i].val_p);
                if (!node || !node->IsCentralNode()) continue;
                ImGuiDockNode* root = ImGui::DockNodeGetRootNode(node);
                if (root->IsDockSpace() && root->HostWindow &&
                    root->HostWindow->Viewport == ImGui::GetMainViewport())
                {
                    targetNode = node->ID;
                    break;
                }
            }
        }
        break;
    case EditorPanelDockArea::LeftSidebar:
        targetNode = nodeForWindow("Hierarchy");
        if (!targetNode) targetNode = nodeForWindow("Assets");
        break;
    case EditorPanelDockArea::RightSidebar:
        targetNode = nodeForWindow("Properties");
        break;
    case EditorPanelDockArea::BottomPanel:
        targetNode = nodeForWindow("Console");
        if (!targetNode) targetNode = nodeForWindow("Problems");
        if (!targetNode) targetNode = nodeForWindow("Terminal");
        break;
    default:
        break;
    }

    if (targetNode)
        ImGui::DockBuilderDockWindow(title, targetNode);
}
}
