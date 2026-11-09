#include "pch.h"
#include "SceneLoadWarningPopup.h"

#include "Engine/Editor/State/EditorState.h"
#include "imgui.h"

namespace Engine::Editor
{
void SceneLoadWarningPopup::Draw(EditorState& state)
{
    if (state.HasPendingSceneLoadWarning())
        ImGui::OpenPopup("Unsaved Scene Changes");

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (!ImGui::BeginPopupModal("Unsaved Scene Changes", nullptr, flags))
        return;

    const std::string pendingScene = state.GetPendingSceneLoadPath();
    const std::string sceneName = pendingScene.empty()
        ? std::string("this scene")
        : std::filesystem::path(pendingScene).filename().string();

    ImGui::TextWrapped("Loading %s will discard unsaved changes in the current scene.",
        sceneName.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Save before switching, or discard the changes and continue.");

    ImGui::Spacing();
    if (ImGui::Button("Save"))
    {
        if (state.ConfirmSceneLoad(true))
            ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard"))
    {
        state.ConfirmSceneLoad(false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        state.CancelSceneLoad();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
}