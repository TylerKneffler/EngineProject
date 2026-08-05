#include "pch.h"
#include "Engine/Editor/UI/ImGui/Layout/ImGuiDockspace.h"
#include "imgui.h"
#include "imgui_internal.h"

void ImGuiDockspace::Draw()
{
    const ImGuiID dockspaceId =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Build defaults only when imgui.ini did not restore a dock tree.
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (root != nullptr && !root->IsLeafNode()) return;

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

    ImGui::DockBuilderDockWindow("Hierarchy 1", left);
    ImGui::DockBuilderDockWindow("Assets 1", left);
    ImGui::DockBuilderDockWindow("Scene 1", centerTop);
    ImGui::DockBuilderDockWindow("Game 1", centerTop);
    ImGui::DockBuilderDockWindow("Console 1", centerBottom);
    ImGui::DockBuilderDockWindow("Properties 1", right);
    ImGui::DockBuilderFinish(dockspaceId);
}
