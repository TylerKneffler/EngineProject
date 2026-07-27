#include "pch.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"

EditorUI::EditorUI(EditorState* state)
    : m_state(state)
{
    OutputDebugStringA("[EditorUI] Constructor called\n");
}

void EditorUI::Render(PlayState playState)
{
    if (!m_state) return;
    m_dockspace.Draw();
    m_mainMenu.Draw(*m_state, playState, m_gameBuildManager);
    m_panelHost.Draw(*m_state);
}
