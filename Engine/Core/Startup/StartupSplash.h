#pragma once

#include "Core/Startup/WindowPlacementStore.h"

#include <Windows.h>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <mutex>
#include <string>
#include <thread>

namespace Engine::Core
{
// A small native window shown while the renderer and scene initialize on the
// main thread. It runs its own message loop on a separate thread, so it stays
// responsive and can be dragged (anywhere on it) during long startup stages;
// the main window then opens where the splash was left (PlaceWindow).
// With a placement key it opens centred on where that main window was last
// left (WindowPlacementStore), otherwise on the monitor under the mouse.
// Background launches
// (LaunchActivation) show it without focus and never topmost.
class StartupSplash
{
public:
    StartupSplash(HINSTANCE instance, std::wstring title,
        const std::wstring& backgroundPath = {}, std::wstring placementKey = {});
    ~StartupSplash();
    StartupSplash(const StartupSplash&) = delete;
    StartupSplash& operator=(const StartupSplash&) = delete;

    void SetProgress(float fraction, std::wstring status);
    // Positions the hidden main window before it is shown and returns the
    // show command to use. It goes back to its saved placement (size too
    // when restoreSize, and maximized), unless the user dragged the splash:
    // then it is centred on the splash, on the splash's monitor.
    int PlaceWindow(HWND window, bool restoreSize) const;
    void HoldAboveMainWindow();
    // Closes the splash, or, while shaders are compiling (ShaderCompileProgress),
    // leaves it up showing them until they have been quiet for a moment; the
    // main window behind it would only show a frozen frame meanwhile.
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam);
    void ThreadMain(HINSTANCE instance);
    void Paint(HDC dc);
    void OnTimer(HWND window);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_created;
    bool m_ready = false;
    HWND m_window = nullptr;
    std::wstring m_title;
    std::wstring m_status = L"Starting...";
    std::wstring m_backgroundPath;
    std::optional<SavedWindowPlacement> m_saved;
    std::atomic<bool> m_userMoved{ false };
    bool m_closeWhenIdle = false;   // splash thread only
    bool m_showedCompile = false;   // splash thread only
    float m_progress = 0.0f;
    ULONG_PTR m_gdiplusToken = 0;
    void* m_background = nullptr;
};
}
