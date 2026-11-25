#include "Core/Startup/StartupSplash.h"
#include "Core/Startup/LaunchActivation.h"
#include "Core/Graphics/ShaderCompileProgress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <gdiplus.h>

namespace Engine::Core
{
namespace
{
constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr wchar_t kClassName[] = L"EngineStartupSplash";
constexpr UINT_PTR kCompileTimer = 1;
constexpr UINT kCloseWhenIdle = WM_APP + 1;
// Quiet time after the last compile before a closing splash goes.
constexpr double kCloseIdleSeconds = 0.75;

std::wstring Clock(double seconds)
{
    const int total = static_cast<int>(std::max(0.0, seconds));
    wchar_t text[16];
    std::swprintf(text, 16, L"%d:%02d", total / 60, total % 60);
    return text;
}
}

StartupSplash::StartupSplash(HINSTANCE instance, std::wstring title,
    const std::wstring& backgroundPath, std::wstring placementKey)
    : m_title(std::move(title)), m_backgroundPath(backgroundPath),
      m_saved(LoadWindowPlacement(placementKey))
{
    m_thread = std::thread([this, instance] { ThreadMain(instance); });
    std::unique_lock lock(m_mutex);
    m_created.wait(lock, [this] { return m_ready; });
}

void StartupSplash::ThreadMain(HINSTANCE instance)
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
    windowClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon)
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    // Centre on where the main window was last left; with nothing saved,
    // on the monitor under the mouse.
    POINT center{};
    if (m_saved)
        center = { (m_saved->bounds.left + m_saved->bounds.right) / 2,
            (m_saved->bounds.top + m_saved->bounds.bottom) / 2 };
    else
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        RECT work{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor))
            work = monitor.rcWork;
        center = { (work.left + work.right) / 2, (work.top + work.bottom) / 2 };
    }
    const int x = center.x - kWidth / 2;
    const int y = center.y - kHeight / 2;
    // Never activated: activating this thread's window makes Windows send
    // activation messages to the main thread's (hidden) window too, and the
    // main thread does not pump messages while it loads, so a click or drag
    // here would hang the splash ("Not Responding"). It also keeps a click
    // on the splash from taking focus. Dragging still works (WM_NCHITTEST).
    HWND window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE, kClassName, m_title.c_str(),
        WS_POPUP | WS_BORDER, x, y, kWidth, kHeight,
        nullptr, nullptr, instance, this);
    if (window)
    {
        // Keep the whole splash on the chosen monitor.
        MoveWindowCentered(window, center, { kWidth, kHeight });
        ShowStartupWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
        SetTimer(window, kCompileTimer, 250, nullptr);
    }
    {
        std::lock_guard lock(m_mutex);
        m_window = window;
        m_ready = true;
    }
    m_created.notify_all();

    MSG message{};
    while (window && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    delete static_cast<Gdiplus::Image*>(m_background);
    m_background = nullptr;
    if (m_gdiplusToken)
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
    m_gdiplusToken = 0;
}

void StartupSplash::SetProgress(float fraction, std::wstring status)
{
    HWND window = nullptr;
    {
        std::lock_guard lock(m_mutex);
        m_progress = std::clamp(fraction, 0.0f, 1.0f);
        m_status = std::move(status);
        window = m_window;
    }
    if (window)
        InvalidateRect(window, nullptr, FALSE);
}

int StartupSplash::PlaceWindow(HWND window, bool restoreSize) const
{
    RECT current{};
    if (!window || !GetWindowRect(window, &current))
        return SW_SHOWDEFAULT;
    SIZE size{ current.right - current.left, current.bottom - current.top };
    POINT center{ (current.left + current.right) / 2, (current.top + current.bottom) / 2 };
    bool maximize = false;
    if (m_saved)
    {
        if (restoreSize)
            size = { m_saved->bounds.right - m_saved->bounds.left,
                m_saved->bounds.bottom - m_saved->bounds.top };
        center = { (m_saved->bounds.left + m_saved->bounds.right) / 2,
            (m_saved->bounds.top + m_saved->bounds.bottom) / 2 };
        maximize = restoreSize && m_saved->maximized;
    }
    RECT splash{};
    if ((m_userMoved || !m_saved) && m_window && GetWindowRect(m_window, &splash))
    {
        center = { (splash.left + splash.right) / 2, (splash.top + splash.bottom) / 2 };
        MoveWindowCentered(window, center, size);
    }
    else
        // Exactly where it was left (re-centring would drift by rounding).
        MoveWindowClamped(window, { m_saved->bounds.left, m_saved->bounds.top,
            m_saved->bounds.left + size.cx, m_saved->bounds.top + size.cy });
    if (!maximize)
        return SW_SHOWDEFAULT;
    if (!IsBackgroundLaunch())
        return SW_SHOWMAXIMIZED;
    // A real maximize would take focus: fill the monitor's work area instead
    // (saved back as maximized, see SaveWindowPlacement).
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor))
        SetWindowPos(window, nullptr, monitor.rcWork.left, monitor.rcWork.top,
            monitor.rcWork.right - monitor.rcWork.left,
            monitor.rcWork.bottom - monitor.rcWork.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return SW_SHOWDEFAULT;
}

void StartupSplash::HoldAboveMainWindow()
{
    // Topmost would cover the app the user is working in.
    if (m_window && !IsBackgroundLaunch())
        SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void StartupSplash::Close()
{
    // The splash thread closes it once no shader is compiling (OnTimer).
    HWND window = nullptr;
    {
        std::lock_guard lock(m_mutex);
        window = m_window;
    }
    if (window)
        PostMessageW(window, kCloseWhenIdle, 0, 0);
}

StartupSplash::~StartupSplash()
{
    HWND window = nullptr;
    {
        std::lock_guard lock(m_mutex);
        window = m_window;
    }
    if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (m_thread.joinable())
        m_thread.join();
}

void StartupSplash::OnTimer(HWND window)
{
    const auto compile = Engine::Graphics::ShaderCompileProgress::Get();
    if (m_closeWhenIdle && compile.active == 0 && compile.idleSeconds >= kCloseIdleSeconds)
    {
        DestroyWindow(window);
        return;
    }
    // Repaint while a compile shows (its clock ticks), and once more after.
    if (compile.active > 0 || m_showedCompile)
        InvalidateRect(window, nullptr, FALSE);
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
    switch (message)
    {
    case WM_NCHITTEST:
        // Drag from anywhere on the splash.
        return HTCAPTION;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_EXITSIZEMOVE:
        if (self)
            self->m_userMoved = true;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (self)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            self->Paint(dc);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_TIMER:
        if (self && wParam == kCompileTimer)
        {
            self->OnTimer(window);
            return 0;
        }
        break;
    case kCloseWhenIdle:
        if (self)
        {
            self->m_closeWhenIdle = true;
            self->OnTimer(window);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kCompileTimer);
        if (self)
        {
            std::lock_guard lock(self->m_mutex);
            self->m_window = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void StartupSplash::Paint(HDC dc)
{
    std::wstring title, status;
    float progress = 0.f;
    {
        std::lock_guard lock(m_mutex);
        title = m_title;
        status = m_status;
        progress = m_progress;
    }
    // Draw off screen, then copy, so frequent progress updates do not flicker.
    Gdiplus::Bitmap buffer(kWidth, kHeight, PixelFormat32bppPARGB);
    Gdiplus::Graphics graphics(&buffer);
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
    graphics.DrawString(title.c_str(), -1, &titleFont,
        Gdiplus::PointF(30.0f, 242.0f), &white);
    graphics.DrawString(status.c_str(), -1, &statusFont,
        Gdiplus::PointF(30.0f, 287.0f), &muted);

    // Progress bar: the caller supplies fractions for completed startup stages.
    Gdiplus::SolidBrush track(Gdiplus::Color(255, 65, 78, 96));
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 95, 188, 237));
    graphics.FillRectangle(&track, 30, 328, kWidth - 60, 7);
    graphics.FillRectangle(&fill, 30, 328,
        static_cast<int>((kWidth - 60) * progress), 7);

    // Shader compiles (ShaderCompileProgress): the one running longest, its
    // time against how long it took last time, and a thin bar for that.
    const auto compile = Engine::Graphics::ShaderCompileProgress::Get();
    m_showedCompile = compile.active > 0;
    if (compile.active > 0)
    {
        std::wstring line = L"Compiling shader " +
            std::wstring(compile.label.begin(), compile.label.end()) + L"  " + Clock(compile.elapsedSeconds);
        if (compile.expectedSeconds > 0.0)
            line += L" of ~" + Clock(compile.expectedSeconds);
        if (compile.active > 1)
            line += L"  (+" + std::to_wstring(compile.active - 1) + L" more)";
        if (compile.finished > 0)
            line += L"  |  " + std::to_wstring(compile.finished) + L" compiled";
        Gdiplus::Font compileFont(&family, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        graphics.DrawString(line.c_str(), -1, &compileFont, Gdiplus::PointF(30.0f, 306.0f), &muted);
        const int barWidth = kWidth - 60;
        Gdiplus::SolidBrush compileFill(Gdiplus::Color(255, 237, 188, 95));
        graphics.FillRectangle(&track, 30, 340, barWidth, 3);
        if (compile.expectedSeconds > 0.0)
        {
            // Past the expected time it holds near the end rather than overflowing.
            const double share = std::min(compile.elapsedSeconds / compile.expectedSeconds, 0.97);
            graphics.FillRectangle(&compileFill, 30, 340, static_cast<int>(barWidth * share), 3);
        }
        else
        {
            // Unknown length: a segment sweeping across, so it reads as alive.
            const int segment = barWidth / 5;
            const double phase = std::fmod(compile.elapsedSeconds / 2.0, 1.0);
            const int x = static_cast<int>((barWidth + segment) * phase) - segment;
            const int left = std::max(x, 0), right = std::min(x + segment, barWidth);
            if (right > left)
                graphics.FillRectangle(&compileFill, 30 + left, 340, right - left, 3);
        }
    }
    Gdiplus::Graphics(dc).DrawImage(&buffer, 0, 0, kWidth, kHeight);
}
}
