#pragma once

#include "Engine/Editor/UI/ImGui/ImGuiEditorUi.h"

namespace Engine::Editor
{
// ImGui panel-hosting feature.

class EditorState;
class GameBuildManager;
enum class PlayState;

// Draws package-neutral editor panels through ImGui and owns their close-time
// resource cleanup plus the Project Preferences window.
class ImGuiPanelHost
{
public:
    void Draw(EditorState& state);
    void DrawToolbar(EditorState& state, PlayState playState,
        GameBuildManager* buildManager);

private:
    void DrawPanels(EditorState& state);
    void DrawPreferences(EditorState& state);

    ImGuiEditorUi m_ui;
};
}
