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
};

NativeInputSnapshot CaptureNativeInput(HWND window);
void SetCurrentNativeInputSnapshot(NativeInputSnapshot* snapshot);
NativeInputSnapshot* CurrentNativeInputSnapshot();
SHORT ReadNativeKeyState(int virtualKey);
bool ReadNativeCursorPosition(POINT& cursor);
bool IsNativeInputFocused();
void RequestNativeCursorPosition(POINT cursor);
void RequestNativeCursorVisible(bool visible);
void ApplyNativeCursorRequests(const NativeInputSnapshot& snapshot);
}
