#include "pch.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/ProjectLoader.h"
#include "Core/Renderers/RendererFactory.h"

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
#endif
#ifndef GAME_EXECUTABLE_PATH
#define GAME_EXECUTABLE_PATH "build/Debug/Debug/Game.exe"
#endif

// Forward declaration
static HANDLE StartGameBuild(HANDLE& outReadPipe);

// ---------------------------------------------------------------------------
// GameBuildManager::~GameBuildManager
// ---------------------------------------------------------------------------
GameBuildManager::~GameBuildManager()
{
    // Ensure the build process is cleaned up
    if (m_buildProcess)
    {
        TerminateProcess(m_buildProcess, 1);
        CloseHandle(m_buildProcess);
    }
    if (m_buildPipe)
        CloseHandle(m_buildPipe);
}

// ---------------------------------------------------------------------------
// GameBuildManager::StartBuild
// ---------------------------------------------------------------------------
void GameBuildManager::StartBuild(PostBuildAction action)
{
    if (m_buildProcess)
        return; // Already building

    if (!ValidateRendererPrerequisites())
    {
        m_playState = PlayState::BuildFailed;
        m_postBuildAction = PostBuildAction::Nothing;
        if (OnBuildComplete) OnBuildComplete(false);
        return;
    }

    // Start the background build process
    m_buildProcess = StartGameBuild(m_buildPipe);
    if (m_buildProcess)
    {
        m_playState = PlayState::Building;
        m_postBuildAction = action;
        m_buildLineBuffer.clear();
        if (OnBuildStart)
            OnBuildStart();
    }
    else
    {
        m_playState = PlayState::BuildFailed;
    }
}

bool GameBuildManager::ValidateRendererPrerequisites()
{
    for (const auto& option : RendererFactory::GetRendererOptions())
    {
        const std::string line = "[Build][Renderer] " + option.name + ": " +
            (option.available ? "available" : "UNAVAILABLE - " + option.unavailableReason);
        OutputDebugStringA((line + "\n").c_str());
        if (m_console)
            m_console->AddLog(option.available ? ConsoleView::Level::Info : ConsoleView::Level::Warning, line);
    }

    try
    {
        ProjectSettings settings = m_developmentSettings;
        if (!m_projectFilePath.empty())
        {
            ProjectLoader loader;
            settings = loader.LoadProject(m_projectFilePath);
        }
        std::string reason;
        if (!RendererFactory::IsRendererAvailable(settings.gameRenderingAPI, &reason))
        {
            const std::string error = "[Build][Renderer] Cannot build/run with " +
                settings.gameRenderingAPI + ": " + reason;
            OutputDebugStringA((error + "\n").c_str());
            if (m_console) m_console->AddLog(ConsoleView::Level::Error, error);
            return false;
        }

        const std::string selected = "[Build][Renderer] Selected game renderer: " + settings.gameRenderingAPI;
        OutputDebugStringA((selected + "\n").c_str());
        if (m_console) m_console->AddLog(ConsoleView::Level::Info, selected);
        return true;
    }
    catch (const std::exception& error)
    {
        const std::string message = "[Build][Renderer] Failed to validate project renderer settings: " +
            std::string(error.what());
        OutputDebugStringA((message + "\n").c_str());
        if (m_console) m_console->AddLog(ConsoleView::Level::Error, message);
        return false;
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::CancelBuild
// ---------------------------------------------------------------------------
void GameBuildManager::CancelBuild()
{
    if (!m_buildProcess)
        return;

    TerminateProcess(m_buildProcess, 1);
    CloseHandle(m_buildProcess);
    m_buildProcess = nullptr;

    if (m_buildPipe)
    {
        CloseHandle(m_buildPipe);
        m_buildPipe = nullptr;
    }

    m_buildLineBuffer.clear();
    m_playState = PlayState::Stopped;
    m_postBuildAction = PostBuildAction::Nothing;

    if (m_console)
        m_console->AddLog(ConsoleView::Level::Warning, "[Build] Build cancelled.");
}

// ---------------------------------------------------------------------------
// GameBuildManager::Stop
// ---------------------------------------------------------------------------
void GameBuildManager::Stop()
{
    if (m_playState == PlayState::Playing || m_playState == PlayState::Paused)
    {
        m_playState = PlayState::Stopped;
        m_postBuildAction = PostBuildAction::Nothing;
        if (OnPlayStop)
            OnPlayStop();
        if (m_console)
            m_console->AddLog(ConsoleView::Level::Info, "[Play] Stopped.");
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::Pause
// ---------------------------------------------------------------------------
void GameBuildManager::Pause()
{
    if (m_playState == PlayState::Playing)
    {
        m_playState = PlayState::Paused;
        if (m_console)
            m_console->AddLog(ConsoleView::Level::Info, "[Play] Paused.");
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::Resume
// ---------------------------------------------------------------------------
void GameBuildManager::Resume()
{
    if (m_playState == PlayState::Paused)
    {
        m_playState = PlayState::Playing;
        if (m_console)
            m_console->AddLog(ConsoleView::Level::Info, "[Play] Resumed.");
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::LaunchStandalone
// ---------------------------------------------------------------------------
void GameBuildManager::LaunchStandalone()
{
    std::string path = GAME_EXECUTABLE_PATH;
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (CreateProcessA(nullptr, buf.data(),
                       nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::Update
// ---------------------------------------------------------------------------
void GameBuildManager::Update(PlayState& outState, PostBuildAction& outAction)
{
    outState = m_playState;
    outAction = m_postBuildAction;

    if (m_playState == PlayState::Building)
    {
        DrainBuildPipe();
        PollBuildProcess();
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::DrainBuildPipe
// ---------------------------------------------------------------------------
void GameBuildManager::DrainBuildPipe()
{
    if (!m_buildPipe || !m_console)
        return;

    DWORD available = 0;
    while (PeekNamedPipe(m_buildPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
    {
        char tmp[512];
        DWORD toRead = available < sizeof(tmp) ? available : sizeof(tmp);
        DWORD bytesRead = 0;
        if (!ReadFile(m_buildPipe, tmp, toRead, &bytesRead, nullptr) || bytesRead == 0)
            break;

        // Split raw bytes into lines and forward to the console.
        for (DWORD i = 0; i < bytesRead; ++i)
        {
            if (tmp[i] == '\n')
            {
                // Strip trailing \r if present.
                if (!m_buildLineBuffer.empty() && m_buildLineBuffer.back() == '\r')
                    m_buildLineBuffer.pop_back();
                if (!m_buildLineBuffer.empty())
                    m_console->AddLog(ConsoleView::Level::Build, m_buildLineBuffer);
                m_buildLineBuffer.clear();
            }
            else
            {
                m_buildLineBuffer += tmp[i];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// GameBuildManager::PollBuildProcess
// ---------------------------------------------------------------------------
void GameBuildManager::PollBuildProcess()
{
    if (!m_buildProcess)
        return;

    if (WaitForSingleObject(m_buildProcess, 0) == WAIT_TIMEOUT)
        return; // Still running

    DWORD exitCode = 1;
    GetExitCodeProcess(m_buildProcess, &exitCode);
    CloseHandle(m_buildProcess);
    m_buildProcess = nullptr;

    // Flush any remaining partial line from the pipe.
    if (!m_buildLineBuffer.empty() && m_console)
    {
        m_console->AddLog(ConsoleView::Level::Build, m_buildLineBuffer);
        m_buildLineBuffer.clear();
    }
    if (m_buildPipe)
    {
        CloseHandle(m_buildPipe);
        m_buildPipe = nullptr;
    }

    HandleBuildCompletion(exitCode == 0);
}

// ---------------------------------------------------------------------------
// GameBuildManager::HandleBuildCompletion
// ---------------------------------------------------------------------------
void GameBuildManager::HandleBuildCompletion(bool success)
{
    if (success)
    {
        switch (m_postBuildAction)
        {
            case PostBuildAction::PlayInEditor:
                m_playState = PlayState::Playing;
                if (OnPlayStart)
                    OnPlayStart();
                if (m_console)
                    m_console->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded. Running in editor.");
                break;
            case PostBuildAction::LaunchStandalone:
                LaunchStandalone();
                m_playState = PlayState::Stopped;
                if (m_console)
                    m_console->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded. Launching standalone.");
                break;
            case PostBuildAction::Nothing:
            default:
                m_playState = PlayState::Stopped;
                if (m_console)
                    m_console->AddLog(ConsoleView::Level::Info, "[Build] Build succeeded.");
                break;
        }
    }
    else
    {
        m_playState = PlayState::BuildFailed;
        if (m_console)
            m_console->AddLog(ConsoleView::Level::Error, "[Build] Build FAILED.");
    }

    if (OnBuildComplete)
        OnBuildComplete(success);

    m_postBuildAction = PostBuildAction::Nothing;
}

// ---------------------------------------------------------------------------
// StartGameBuild (from Main.cpp)
// ---------------------------------------------------------------------------
static HANDLE StartGameBuild(HANDLE& outReadPipe)
{
    outReadPipe = nullptr;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return nullptr;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    std::string cmd =
        "cmake --build \"" ENGINE_BUILD_DIR
        "\" --config Debug --target Game --parallel";

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = nullptr;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, buf.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return nullptr;
    }

    CloseHandle(hWrite);
    CloseHandle(pi.hThread);
    outReadPipe = hRead;
    return pi.hProcess;
}
