#include "Core/Startup/LaunchActivation.h"

#include <TlHelp32.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Engine::Core
{
namespace
{
// -1 = not decided yet, 0 = normal, 1 = background.
std::atomic<int> g_background{ -1 };

DWORD ParentProcessId(DWORD process)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID == process)
        {
            parent = entry.th32ParentProcessID;
            break;
        }
    CloseHandle(snapshot);
    return parent;
}

// True when the parent or grandparent process owns the foreground window.
bool LaunchedFromForegroundApp()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(foreground, &foregroundProcess);
    if (!foregroundProcess)
        return false;
    DWORD ancestor = GetCurrentProcessId();
    for (int level = 0; level < 2; ++level)
    {
        ancestor = ParentProcessId(ancestor);
        if (!ancestor)
            return false;
        if (ancestor == foregroundProcess)
            return true;
    }
    return false;
}

bool DetectBackgroundLaunch()
{
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "ENGINE_LAUNCH_BACKGROUND") == 0 && value)
    {
        const bool background = value[0] && std::strcmp(value, "0") != 0 &&
            _stricmp(value, "false") != 0 && _stricmp(value, "off") != 0;
        std::free(value);
        return background;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    if (startup.dwFlags & STARTF_USESHOWWINDOW)
    {
        switch (startup.wShowWindow)
        {
        case SW_SHOWNOACTIVATE:
        case SW_SHOWNA:
        case SW_SHOWMINNOACTIVE:
        case SW_MINIMIZE:
        case SW_HIDE:
            return true;
        default:
            // Explorer, shortcuts and `start` ask for an activating show.
            return false;
        }
    }
    // Launched from the window the user is in (Explorer, an IDE's F5, the
    // Editor starting the Game): a focused launch. Shells and scripts behind
    // another app, test runners and agents start in the background.
    return !LaunchedFromForegroundApp();
}
}

bool IsBackgroundLaunch()
{
    int state = g_background.load();
    if (state < 0)
    {
        int detected = DetectBackgroundLaunch() ? 1 : 0;
        // A concurrent SetBackgroundLaunch wins over detection.
        g_background.compare_exchange_strong(state, detected);
        state = g_background.load();
    }
    return state == 1;
}

void SetBackgroundLaunch(bool background)
{
    g_background = background ? 1 : 0;
}

bool DisablePowerThrottling()
{
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // Controlled flags with a clear state bit: always run at high QoS and
    // honour timer resolution, whatever window has focus.
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
        &state, sizeof(state)))
        return true;
    // Older Windows 10 builds reject the timer-resolution flag.
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
        &state, sizeof(state)) != FALSE;
}

bool IsAnotherProcessInForeground()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    return process != GetCurrentProcessId();
}

void ShowStartupWindow(HWND window, int showCommand)
{
    if (!window)
        return;
    if (!IsBackgroundLaunch())
    {
        ShowWindow(window, showCommand);
        return;
    }
    // Show it already in place behind the app in use: ShowWindow would put
    // the new window on top first, and even one frame over a fullscreen game
    // takes that game out of fullscreen.
    HWND foreground = GetForegroundWindow();
    const bool behind = foreground && foreground != window && IsAnotherProcessInForeground();
    SetWindowPos(window, behind ? foreground : HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW |
        (behind ? 0u : static_cast<UINT>(SWP_NOZORDER)));
}
}
