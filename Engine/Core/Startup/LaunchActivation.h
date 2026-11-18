#pragma once

#include <Windows.h>

namespace Engine::Core
{
// Whether this process's startup windows (splash, main window, editor panel
// windows) should appear without taking focus from the app the user is
// working in. True when:
//   - SetBackgroundLaunch(true) was called (GameBackendCapture does by default),
//   - ENGINE_LAUNCH_BACKGROUND=1 (0 forces a normal, focused launch), or
//   - the launcher asked for a no-activate show state in STARTUPINFO.
// Otherwise windows open focused, as before.
bool IsBackgroundLaunch();
void SetBackgroundLaunch(bool background);

// Shows a top-level window at startup. Normal launches use showCommand
// (SW_SHOWDEFAULT honours the launcher's show state). Background launches show
// it inactive and place it just behind the foreground window when that window
// belongs to another process, so typing there is not interrupted.
void ShowStartupWindow(HWND window, int showCommand = SW_SHOWDEFAULT);

// True while another process owns the foreground window.
bool IsAnotherProcessInForeground();
}
