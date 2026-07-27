#pragma once

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
    void DrawPlayControls(PlayState playState,
        GameBuildManager* buildManager) const;
    void OpenPanel(EditorState& state, const char* type) const;
};
