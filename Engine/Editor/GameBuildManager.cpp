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

namespace
{
size_t CountFiles(const std::string& directory,
    std::initializer_list<const char*> extensions)
{
    if (directory.empty()) return 0;
    std::error_code error;
    size_t count = 0;
    for (std::filesystem::recursive_directory_iterator iterator(directory, error), end;
         iterator != end && !error; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error)) continue;
        std::string extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        for (const char* expected : extensions)
            if (extension == expected) { ++count; break; }
    }
    return count;
}
}

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

    if (m_console)
    {
        const size_t scripts = CountFiles(m_developmentSettings.scriptsDirectory,
            {".cpp", ".c", ".cc", ".cxx"});
        const size_t shaders = CountFiles(m_developmentSettings.shadersDirectory,
            {".hlsl", ".glsl", ".vert", ".frag", ".comp"});
        const size_t assets = CountFiles(m_developmentSettings.assetsDirectory,
            {".png", ".jpg", ".jpeg", ".dds", ".obj", ".fbx", ".gltf", ".glb", ".wav"});
        m_console->AddLog(ConsoleView::Level::Build,
            "[Build] Starting Debug Game build: cmake configure/check, compile, and link.");
        m_console->AddLog(ConsoleView::Level::Build,
            "[Scripts] " + std::to_string(scripts) + " project source file(s) queued for the Game target.");
        m_console->AddLog(ConsoleView::Level::Build,
            "[Shaders] " + std::to_string(shaders) + " shader source file(s) discovered; renderer compilation occurs at startup.");
        m_console->AddLog(ConsoleView::Level::Build,
            "[Assets] " + std::to_string(assets) + " runtime asset file(s) discovered.");
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
        if (m_console)
            m_console->AddLog(ConsoleView::Level::Error,
                "[Build] Could not start the CMake build process.");
        if (OnBuildComplete) OnBuildComplete(false);
    }
}

void GameBuildManager::PlayInEditor()
{
    if (m_buildProcess || m_playState == PlayState::Playing ||
        m_playState == PlayState::Paused)
        return;

    m_postBuildAction = PostBuildAction::Nothing;
    m_playState = PlayState::Playing;
    if (OnPlayStart)
        OnPlayStart();
    if (m_console)
        m_console->AddLog(ConsoleView::Level::Info, "[Play] Running in editor.");
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
    if (m_playState == PlayState::Building)
    {
        DrainBuildPipe();
        PollBuildProcess();
    }

    // Publish state after polling so build completion and play transitions are
    // visible to the editor during this frame rather than one frame later.
    outState = m_playState;
    outAction = m_postBuildAction;
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
        "\" --config Debug --target Game --parallel 1";

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
