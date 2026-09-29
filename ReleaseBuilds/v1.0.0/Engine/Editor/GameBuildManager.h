#pragma once
#include "pch.h"
#include "Core/ProjectLoader.h"
#include "Engine/Editor/BuildTreeLock.h"
#include <functional>

namespace Engine::Editor
{
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
        Engine::Model::ProjectSettings developmentSettings = {})
        : m_projectFilePath(std::move(projectFilePath))
        , m_developmentSettings(std::move(developmentSettings))
        , m_console(console) {}
    ~GameBuildManager();

    // ---- Build Control ----
    void StartBuild(PostBuildAction action);
    void PlayInEditor();
    void CancelBuild();
    void Stop();
    void Pause();
    void Resume();
    void LaunchStandalone();

    // ---- Frame Update ----
    void Update(PlayState& outState, PostBuildAction& outAction);

    // ---- State Queries ----
    bool IsBuilding() const { return m_buildProcess != nullptr || m_buildQueued; }
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
    void TryStartQueuedBuild();

    // Build process handles
    HANDLE m_buildProcess = nullptr;
    HANDLE m_buildPipe = nullptr;
    HANDLE m_buildJob = nullptr;
    std::string m_buildLineBuffer;
    BuildTreeLock m_buildTreeLock;
    bool m_buildQueued = false;
    bool m_waitingForBuildTree = false;

    // State tracking
    PlayState m_playState = PlayState::Stopped;
    PostBuildAction m_postBuildAction = PostBuildAction::Nothing;
    std::string m_projectFilePath;
    Engine::Model::ProjectSettings m_developmentSettings;

    // References
    ConsoleView* m_console = nullptr;
};
}
