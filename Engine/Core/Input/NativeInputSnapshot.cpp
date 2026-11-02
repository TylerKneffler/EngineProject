#include "Core/Input/NativeInputSnapshot.h"
#include <algorithm>

namespace Engine::Core
{
namespace
{
thread_local NativeInputSnapshot* currentSnapshot = nullptr;

void SetSystemCursorVisible(bool visible)
{
    CURSORINFO info{ sizeof(CURSORINFO) };
    if (GetCursorInfo(&info) && ((info.flags & CURSOR_SHOWING) != 0) == visible)
        return;
    if (visible)
        while (ShowCursor(TRUE) < 0) {}
    else
        while (ShowCursor(FALSE) >= 0) {}
}
}

NativeInputSnapshot CaptureNativeInput(HWND window)
{
    NativeInputSnapshot snapshot;
    const HWND foreground = GetForegroundWindow();
    snapshot.focused = window && foreground &&
        GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT);
    if (snapshot.focused)
        for (int key = 0; key < 256; ++key)
            snapshot.keys[static_cast<size_t>(key)] = GetAsyncKeyState(key);
    GetCursorPos(&snapshot.cursor);
    RECT client{};
    if (window && GetClientRect(window, &client))
    {
        snapshot.clientWidth = static_cast<uint32_t>(
            std::max(0L, client.right - client.left));
        snapshot.clientHeight = static_cast<uint32_t>(
            std::max(0L, client.bottom - client.top));
        snapshot.clientCursor = snapshot.cursor;
        if (ScreenToClient(window, &snapshot.clientCursor))
            snapshot.cursorInWindow = snapshot.focused &&
                snapshot.clientCursor.x >= client.left &&
                snapshot.clientCursor.y >= client.top &&
                snapshot.clientCursor.x < client.right &&
                snapshot.clientCursor.y < client.bottom;
        snapshot.center = { (client.left + client.right) / 2,
            (client.top + client.bottom) / 2 };
        snapshot.hasCenter = ClientToScreen(window, &snapshot.center) != FALSE;
    }
    return snapshot;
}

void SetCurrentNativeInputSnapshot(NativeInputSnapshot* snapshot)
{
    currentSnapshot = snapshot;
}

NativeInputSnapshot* CurrentNativeInputSnapshot() { return currentSnapshot; }

SHORT ReadNativeKeyState(int virtualKey)
{
    if (currentSnapshot && virtualKey >= 0 && virtualKey < 256)
        return currentSnapshot->keys[static_cast<size_t>(virtualKey)];
    return GetAsyncKeyState(virtualKey);
}

bool ReadNativeCursorPosition(POINT& cursor)
{
    if (currentSnapshot)
    {
        cursor = currentSnapshot->cursor;
        return true;
    }
    return GetCursorPos(&cursor) != FALSE;
}

bool IsNativeInputFocused()
{
    return currentSnapshot ? currentSnapshot->focused : GetFocus() != nullptr;
}

void RequestNativeCursorPosition(POINT cursor)
{
    if (currentSnapshot)
    {
        currentSnapshot->cursorPositionRequested = true;
        currentSnapshot->requestedCursorPosition = cursor;
        currentSnapshot->cursor = cursor;
    }
    else
        SetCursorPos(cursor.x, cursor.y);
}

void RequestNativeCursorVisible(bool visible)
{
    if (currentSnapshot) currentSnapshot->cursorVisible = visible;
    else SetSystemCursorVisible(visible);
}

void ApplyNativeCursorRequests(const NativeInputSnapshot& snapshot)
{
    if (snapshot.cursorPositionRequested)
        SetCursorPos(snapshot.requestedCursorPosition.x,
            snapshot.requestedCursorPosition.y);
    if (snapshot.cursorVisible)
        SetSystemCursorVisible(*snapshot.cursorVisible);
}
}
