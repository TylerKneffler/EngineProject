#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <optional>

namespace Engine::Core
{
// Captured by the window owner after pumping messages. A copy belongs to one
// simulation tick; cursor requests return with that tick's render handoff.
struct NativeInputSnapshot
{
    std::array<SHORT, 256> keys{};
    POINT cursor{};
    POINT clientCursor{};
    POINT center{};
    bool focused = false;
    bool cursorInWindow = false;
    bool hasCenter = false;
    uint32_t clientWidth = 0, clientHeight = 0;
    bool cursorPositionRequested = false;
    POINT requestedCursorPosition{};
    std::optional<bool> cursorVisible;
    // Relative mouse motion since the last capture, in raw device counts
    // (WM_INPUT; no pointer acceleration). Mouse-look reads this instead of
    // the cursor's offset from the centre, which loses whatever moves between
    // the capture and the re-centring after the update. False when no raw
    // input has arrived (registration failed) or a benchmark drives the look.
    bool hasRawMouse = false;
    long rawMouseX = 0, rawMouseY = 0;
};

NativeInputSnapshot CaptureNativeInput(HWND window);
// Window procedures pass WM_INPUT here (Window::WndProc); returns true when
// it was relative mouse motion and was taken into the next capture.
bool AccumulateRawMouseInput(LPARAM rawInput);
// Mouse-look's motion this tick: raw counts when the snapshot has them, else
// false (callers fall back to the cursor's offset from their anchor).
bool ReadNativeRawMouseDelta(long& x, long& y);
void SetCurrentNativeInputSnapshot(NativeInputSnapshot* snapshot);
NativeInputSnapshot* CurrentNativeInputSnapshot();
SHORT ReadNativeKeyState(int virtualKey);
bool ReadNativeCursorPosition(POINT& cursor);
bool IsNativeInputFocused();
void RequestNativeCursorPosition(POINT cursor);
void RequestNativeCursorVisible(bool visible);
void ApplyNativeCursorRequests(const NativeInputSnapshot& snapshot);
}
