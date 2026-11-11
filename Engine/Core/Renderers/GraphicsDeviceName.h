#pragma once

#include <string>
#include <windows.h>

namespace Engine::Renderers::Details
{
inline std::string GraphicsDeviceNameFromWide(const wchar_t* name)
{
    if (!name || !*name) return "unknown";
    const int length = WideCharToMultiByte(CP_UTF8, 0, name, -1,
        nullptr, 0, nullptr, nullptr);
    if (length <= 1) return "unknown";
    std::string result(static_cast<size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), length,
            nullptr, nullptr))
        return "unknown";
    result.pop_back();
    return result;
}
}
