#pragma once
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Engine::Editor
{
inline std::string PerformanceJsonString(const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 32)
        {
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        }
        else result += c;
    }
    return result + '"';
}

// Cache process identity once; never query the filesystem on the frame path.
static inline const std::string& PerformanceProvenanceFields()
{
    static const std::string fields = [] {
        std::string executable;
#if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length && length < buffer.size())
        {
            buffer.resize(length);
            const auto utf8 = std::filesystem::path(buffer).generic_u8string();
            executable.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }
#endif
#if defined(ENGINE_BUILD_CONFIGURATION)
        const char* configuration = ENGINE_BUILD_CONFIGURATION;
#else
        const char* configuration = "unknown";
#endif
#if defined(NDEBUG)
        const char* assertions = "false";
#else
        const char* assertions = "true";
#endif
        return "\"executable_path\":" + PerformanceJsonString(executable) +
            ",\"build_configuration\":" + PerformanceJsonString(configuration) +
            ",\"assertions_enabled\":" + assertions;
    }();
    return fields;
}
}
