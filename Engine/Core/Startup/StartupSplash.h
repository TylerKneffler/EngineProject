#pragma once

#include <Windows.h>
#include <string>

namespace Engine::Core
{
// A small native window that remains responsive while the renderer and scene
// are initialized on the main thread. Progress is advanced at real milestones.
class StartupSplash
{
public:
    StartupSplash(HINSTANCE instance, std::wstring title,
        const std::wstring& backgroundPath = {});
    ~StartupSplash();
    StartupSplash(const StartupSplash&) = delete;
    StartupSplash& operator=(const StartupSplash&) = delete;

    void SetProgress(float fraction, std::wstring status);
    void HoldAboveMainWindow();
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam);
    void Paint(HDC dc);
    void PumpMessages();

    HWND m_window = nullptr;
    std::wstring m_title;
    std::wstring m_status = L"Starting...";
    std::wstring m_backgroundPath;
    float m_progress = 0.0f;
    ULONG_PTR m_gdiplusToken = 0;
    void* m_background = nullptr;
};
}
