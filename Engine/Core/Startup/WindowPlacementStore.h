#pragma once

#include <Windows.h>
#include <optional>
#include <string>

namespace Engine::Core
{
// Remembers where an application's main window was last left (screen rect of
// its restored size, and whether it was maximized), per user, in
// %LOCALAPPDATA%\Engine\window-<key>.txt. Startup reopens the window there
// (on the same monitor) and centres the splash on it.
struct SavedWindowPlacement
{
    RECT bounds{};          // screen coordinates of the restored window
    bool maximized = false;
};

// Empty when nothing is saved or the saved rect is on no connected monitor.
std::optional<SavedWindowPlacement> LoadWindowPlacement(const std::wstring& key);
void SaveWindowPlacement(const std::wstring& key, HWND window);

// Moves `window` so it has `size` centred on `center`, clamped to the work
// area of the monitor containing `center` (shrinking only if it is larger).
void MoveWindowCentered(HWND window, POINT center, SIZE size);
// Moves/sizes `window` to `target`, clamped to the work area of the monitor
// containing the target's centre.
void MoveWindowClamped(HWND window, const RECT& target);
}
