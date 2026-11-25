#pragma once

#include <Windows.h>

namespace Engine::Core
{
// Whether this process's startup windows (splash, main window, editor panel
// windows) should appear without taking focus from the app the user is
// working in. Background is the default (test runs, captures and scripted
// launches must not steal focus from the app in use). Decided by, in order:
//   - SetBackgroundLaunch() (GameBackendCapture: background unless --foreground),
//   - ENGINE_LAUNCH_BACKGROUND=1 / =0 (forces background / a focused launch),
//   - the launcher's STARTUPINFO show state: a no-activate state is background,
//     an activating one (Explorer, shortcuts, `start`) is a focused launch,
//   - the user launching from the window they are using: when the parent or
//     grandparent process owns the foreground window (Explorer, Visual Studio
//     F5, the Editor starting the Game) the launch is focused.
// Anything else (shells and scripts behind another app, agents) is background.
bool IsBackgroundLaunch();
void SetBackgroundLaunch(bool background);

// Shows a top-level window at startup. Normal launches use showCommand
// (SW_SHOWDEFAULT honours the launcher's show state). Background launches show
// it inactive, directly behind the foreground window when that window belongs
// to another process (never on top of it, even for a frame: covering a
// fullscreen game knocks it out of fullscreen), so typing there is not
// interrupted.
void ShowStartupWindow(HWND window, int showCommand = SW_SHOWDEFAULT);

// True while another process owns the foreground window.
bool IsAnotherProcessInForeground();

// Opts this process out of Windows power throttling (EcoQoS). Windows 11
// otherwise lowers the QoS of a process whose window is not in the
// foreground: on hybrid CPUs its threads move to efficiency cores and its
// timer-resolution requests are ignored, which slowed every CPU stage of an
// unfocused benchmark 2-3x. Returns false where the OS does not support it.
bool DisablePowerThrottling();
}
