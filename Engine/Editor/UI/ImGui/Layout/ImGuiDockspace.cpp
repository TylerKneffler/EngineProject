#include "pch.h"
#include "Engine/Editor/UI/ImGui/Layout/ImGuiDockspace.h"
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
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID left, center, right;
    ImGui::DockBuilderSplitNode(
        dockspaceId, ImGuiDir_Left, 0.20f, &left, &center);
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right, 0.31f, &right, &center);

    ImGuiID centerTop, centerBottom;
    ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, 0.25f, &centerBottom, &centerTop);

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
}
