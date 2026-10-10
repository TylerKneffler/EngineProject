#include "Core/Startup/StartupSplash.h"

#include <algorithm>
#include <gdiplus.h>

namespace Engine::Core
{
namespace
{
constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr wchar_t kClassName[] = L"EngineStartupSplash";
}

StartupSplash::StartupSplash(HINSTANCE instance, std::wstring title,
    const std::wstring& backgroundPath)
    : m_title(std::move(title)), m_backgroundPath(backgroundPath)
{
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &input, nullptr) == Gdiplus::Ok &&
        !m_backgroundPath.empty())
    {
        auto* image = Gdiplus::Image::FromFile(m_backgroundPath.c_str());
        if (image && image->GetLastStatus() == Gdiplus::Ok)
            m_background = image;
        else
            delete image;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon)
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    const int x = (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2;
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, m_title.c_str(),
        WS_POPUP | WS_BORDER, x, y, kWidth, kHeight,
        nullptr, nullptr, instance, this);
    if (m_window)
    {
        ShowWindow(m_window, SW_SHOWNORMAL);
        UpdateWindow(m_window);
        PumpMessages();
    }
}

StartupSplash::~StartupSplash()
{
    Close();
    delete static_cast<Gdiplus::Image*>(m_background);
    if (m_gdiplusToken)
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
}

void StartupSplash::SetProgress(float fraction, std::wstring status)
{
    m_progress = std::clamp(fraction, 0.0f, 1.0f);
    m_status = std::move(status);
    if (m_window)
    {
        InvalidateRect(m_window, nullptr, FALSE);
        UpdateWindow(m_window);
        PumpMessages();
    }
}

void StartupSplash::HoldAboveMainWindow()
{
    if (m_window)
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void StartupSplash::Close()
{
    if (m_window)
    {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void StartupSplash::PumpMessages()
{
    MSG message{};
    while (m_window && PeekMessageW(&message, m_window, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK StartupSplash::WindowProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<StartupSplash*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_PAINT && self)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        self->Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void StartupSplash::Paint(HDC dc)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    Gdiplus::LinearGradientBrush fallback(
        Gdiplus::Point(0, 0), Gdiplus::Point(kWidth, kHeight),
        Gdiplus::Color(255, 19, 30, 47), Gdiplus::Color(255, 46, 59, 78));
    graphics.FillRectangle(&fallback, 0, 0, kWidth, kHeight);

    // Background: place Assets/Startup/Editor.png or Game.png beside project
    // assets to replace the default gradient. It fills the splash canvas.
    if (m_background)
        graphics.DrawImage(static_cast<Gdiplus::Image*>(m_background),
            Gdiplus::Rect(0, 0, kWidth, kHeight));

    // Overlay: a dark panel keeps title, status, and bar readable over artwork.
    Gdiplus::SolidBrush overlay(Gdiplus::Color(205, 12, 18, 29));
    graphics.FillRectangle(&overlay, 0, 222, kWidth, 138);

    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font titleFont(&family, 22, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font statusFont(&family, 14, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 245, 247, 250));
    Gdiplus::SolidBrush muted(Gdiplus::Color(255, 191, 201, 213));
    graphics.DrawString(m_title.c_str(), -1, &titleFont,
        Gdiplus::PointF(30.0f, 242.0f), &white);
    graphics.DrawString(m_status.c_str(), -1, &statusFont,
        Gdiplus::PointF(30.0f, 287.0f), &muted);

    // Progress bar: the caller supplies fractions for completed startup stages.
    Gdiplus::SolidBrush track(Gdiplus::Color(255, 65, 78, 96));
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 95, 188, 237));
    graphics.FillRectangle(&track, 30, 328, kWidth - 60, 7);
    graphics.FillRectangle(&fill, 30, 328,
        static_cast<int>((kWidth - 60) * m_progress), 7);
}
}
