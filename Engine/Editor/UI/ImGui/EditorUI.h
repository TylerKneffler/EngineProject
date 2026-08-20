#pragma once

#include "Engine/Editor/UI/ImGui/Layout/ImGuiDockspace.h"
#include "Engine/Editor/UI/ImGui/Menus/ImGuiMainMenu.h"
#include "Engine/Editor/UI/ImGui/Panels/ImGuiPanelHost.h"
#include "Engine/Editor/UI/ImGui/Popups/SceneLoadWarningPopup.h"

namespace Engine::Editor
{
class EditorState;
class GameBuildManager;
enum class PlayState;

// Lightweight ImGui editor-presentation orchestrator. Feature-specific drawing
// and state live in the dockspace, menu, and panel-host components.
class EditorUI
{
public:
    explicit EditorUI(EditorState* state);

    void Render(PlayState playState);
    void SetGameBuildManager(GameBuildManager* manager)
    {
        m_gameBuildManager = manager;
    }

private:
    EditorState* m_state = nullptr;
    GameBuildManager* m_gameBuildManager = nullptr;
    ImGuiDockspace m_dockspace;
    ImGuiMainMenu m_mainMenu;
    ImGuiPanelHost m_panelHost;
    SceneLoadWarningPopup m_sceneLoadWarningPopup;
};
}
