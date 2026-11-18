#include "Core/Startup/WindowPlacementStore.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Engine::Core
{
namespace
{
std::filesystem::path PlacementPath(const std::wstring& key)
{
    wchar_t* base = nullptr;
    size_t length = 0;
    std::filesystem::path directory;
    if (_wdupenv_s(&base, &length, L"LOCALAPPDATA") == 0 && base)
    {
        directory = std::filesystem::path(base) / L"Engine";
        std::free(base);
    }
    else
        directory = std::filesystem::temp_directory_path() / L"Engine";
    return directory / (L"window-" + key + L".txt");
}

bool WorkArea(POINT point, RECT& work)
{
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    work = monitor.rcWork;
    return true;
}

POINT Center(const RECT& rect)
{
    return { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
}
}

std::optional<SavedWindowPlacement> LoadWindowPlacement(const std::wstring& key)
{
    if (key.empty())
        return std::nullopt;
    std::ifstream file(PlacementPath(key));
    SavedWindowPlacement placement;
    int maximized = 0;
    if (!(file >> placement.bounds.left >> placement.bounds.top >>
        placement.bounds.right >> placement.bounds.bottom >> maximized))
        return std::nullopt;
    placement.maximized = maximized != 0;
    const LONG width = placement.bounds.right - placement.bounds.left;
    const LONG height = placement.bounds.bottom - placement.bounds.top;
    // Ignore placements on a monitor that is no longer connected.
    if (width < 64 || height < 64 ||
        !MonitorFromRect(&placement.bounds, MONITOR_DEFAULTTONULL))
        return std::nullopt;
    return placement;
}

void SaveWindowPlacement(const std::wstring& key, HWND window)
{
    if (key.empty() || !window)
        return;
    // GetWindowPlacement stays valid while the window is hidden or being
    // destroyed (callers save from WM_DESTROY), and for minimized windows.
    WINDOWPLACEMENT native{};
    native.length = sizeof(native);
    if (!GetWindowPlacement(window, &native))
        return;
    SavedWindowPlacement placement;
    placement.maximized = native.showCmd == SW_SHOWMAXIMIZED ||
        (native.showCmd == SW_SHOWMINIMIZED && (native.flags & WPF_RESTORETOMAXIMIZED));
    // rcNormalPosition is in workspace coordinates, which are offset from
    // screen coordinates by the primary monitor's work-area origin.
    placement.bounds = native.rcNormalPosition;
    MONITORINFO primary{};
    primary.cbSize = sizeof(primary);
    if (GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &primary))
        OffsetRect(&placement.bounds, primary.rcWork.left - primary.rcMonitor.left,
            primary.rcWork.top - primary.rcMonitor.top);
    // A window filling its monitor's work area exactly is treated as
    // maximized (background launches restore maximized windows this way,
    // since a real maximize would take focus).
    RECT work{};
    if (!placement.maximized && WorkArea(Center(placement.bounds), work) &&
        EqualRect(&work, &placement.bounds))
        placement.maximized = true;
    const auto path = PlacementPath(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::trunc);
    file << placement.bounds.left << ' ' << placement.bounds.top << ' '
        << placement.bounds.right << ' ' << placement.bounds.bottom << ' '
        << (placement.maximized ? 1 : 0) << '\n';
}

void MoveWindowCentered(HWND window, POINT center, SIZE size)
{
    MoveWindowClamped(window, { center.x - size.cx / 2, center.y - size.cy / 2,
        center.x - size.cx / 2 + size.cx, center.y - size.cy / 2 + size.cy });
}

void MoveWindowClamped(HWND window, const RECT& target)
{
    RECT work{};
    if (!window || !WorkArea(Center(target), work))
        return;
    const LONG width = std::min<LONG>(target.right - target.left, work.right - work.left);
    const LONG height = std::min<LONG>(target.bottom - target.top, work.bottom - work.top);
    const LONG x = std::clamp<LONG>(target.left, work.left, work.right - width);
    const LONG y = std::clamp<LONG>(target.top, work.top, work.bottom - height);
    RECT current{};
    GetWindowRect(window, &current);
    const bool resized = width != current.right - current.left ||
        height != current.bottom - current.top;
    SetWindowPos(window, nullptr, x, y, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE | (resized ? 0u : SWP_NOSIZE));
}
}
