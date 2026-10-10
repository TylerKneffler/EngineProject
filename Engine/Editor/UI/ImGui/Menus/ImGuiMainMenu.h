#pragma once
#include <string>

namespace Engine::Editor
{
class EditorState;
class GameBuildManager;
enum class PlayState;

// Owns ImGui's editor commands, view menu, and play-state controls.
class ImGuiMainMenu
{
public:
    void Draw(EditorState& state, PlayState playState,
        GameBuildManager* buildManager) const;

private:
    void DrawFileMenu(EditorState& state, PlayState playState,
        GameBuildManager* buildManager) const;
    void DrawViewsMenu(EditorState& state) const;
    void DrawToolbarMenu(EditorState& state) const;
    void DrawRenderingMenu(EditorState& state, PlayState playState) const;
    void OpenPanel(EditorState& state, const char* type) const;
    mutable bool m_openNewAnimationPopup = false;
    mutable char m_newAnimationName[128]{};
    mutable std::string m_newAnimationError;
};
}
