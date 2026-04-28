#pragma once
#include "pch.h"

class EditorState;
class GameBuildManager;
enum class PlayState;

// ---------------------------------------------------------------------------
// EditorUI — Handles all ImGui rendering and UI state
// ---------------------------------------------------------------------------
class EditorUI
{
public:
    EditorUI(EditorState* state);

    // ---- Rendering ----
    void Render(PlayState playState);

    // ---- Setup ----
    void SetGameBuildManager(GameBuildManager* mgr) { m_gameBuildManager = mgr; }

    // ---- Menu Actions ----
    void OnMenuSave();
    void OnMenuBuild();
    void OnMenuBuildAndPlay();
    void OnMenuBuildAndLaunchStandalone();
    void OnMenuPreferences();
    void OnMenuExit();

private:
    void SetupDockingLayout();
    void DrawMenuBar(PlayState playState);
    void DrawPlayControls(PlayState playState);
    void DrawPanels();
    void DrawPreferences();
    void DrawUnsavedChangesModal();

    EditorState* m_state = nullptr;
    GameBuildManager* m_gameBuildManager = nullptr;

    bool m_showUnsavedWarning = false;
    std::string m_sceneToLoadOnConfirm;
};
