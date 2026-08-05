#pragma once

// ImGui panel-hosting feature.

#include "Engine/Editor/UI/ImGui/ImGuiEditorUi.h"

class EditorState;

// Draws package-neutral editor panels through ImGui and owns their close-time
// resource cleanup plus the Project Preferences window.
class ImGuiPanelHost
{
public:
    void Draw(EditorState& state);

private:
    void DrawPanels(EditorState& state);
    void DrawPreferences(EditorState& state);

    ImGuiEditorUi m_ui;
};
