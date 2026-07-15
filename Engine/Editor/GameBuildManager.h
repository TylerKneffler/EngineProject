#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include <functional>

class ConsoleView;

// ---------------------------------------------------------------------------
// PlayState — controls whether game lifecycle (Start/Update) is running.
// ---------------------------------------------------------------------------
enum class PlayState { Stopped, Building, BuildFailed, Playing, Paused };

// What to do once a background build completes.
enum class PostBuildAction { PlayInEditor, LaunchStandalone, Nothing };

// ---------------------------------------------------------------------------
// GameBuildManager — Build process and game execution management
// ---------------------------------------------------------------------------
class GameBuildManager
{
public:
    GameBuildManager(ConsoleView* console, std::string projectFilePath,
        ProjectSettings developmentSettings = {})
        : m_projectFilePath(std::move(projectFilePath))
        , m_developmentSettings(std::move(developmentSettings))
        , m_console(console) {}
    ~GameBuildManager();

    // ---- Build Control ----
    void StartBuild(PostBuildAction action);
    void CancelBuild();
    void Stop();
    void Pause();
    void Resume();
    void LaunchStandalone();

    // ---- Frame Update ----
    void Update(PlayState& outState, PostBuildAction& outAction);

    // ---- State Queries ----
    bool IsBuilding() const { return m_buildProcess != nullptr; }
    PlayState GetPlayState() const { return m_playState; }

    // ---- Callbacks ----
    std::function<void()> OnBuildStart;
    std::function<void(bool success)> OnBuildComplete;
    std::function<void()> OnPlayStart;
    std::function<void()> OnPlayStop;

private:
    bool ValidateRendererPrerequisites();
    void PollBuildProcess();
    void DrainBuildPipe();
    void HandleBuildCompletion(bool success);

    // Build process handles
    HANDLE m_buildProcess = nullptr;
    HANDLE m_buildPipe = nullptr;
    std::string m_buildLineBuffer;

    // State tracking
    PlayState m_playState = PlayState::Stopped;
    PostBuildAction m_postBuildAction = PostBuildAction::Nothing;
    std::string m_projectFilePath;
    ProjectSettings m_developmentSettings;

    // References
    ConsoleView* m_console = nullptr;
};
