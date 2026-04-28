#include "pch.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Core/View/Views/ConsoleView.h"

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ENGINE_BUILD_DIR
#define ENGINE_BUILD_DIR "build/Debug"
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
    std::string path = std::string(ENGINE_BUILD_DIR) + "/Debug/Game.exe";
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
