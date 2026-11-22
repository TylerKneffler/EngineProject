#pragma once

#include <functional>

// Called when a graphics API call fails (ThrowIfFailed and the backends' own
// failure paths), before the exception is thrown. A backend registers a
// function that prints what it knows about a removed device (reason, DRED
// breadcrumbs); without one nothing happens. The argument names the call site.
namespace Engine::Renderers
{
inline std::function<void(const char*)>& GpuFailureHook()
{
    static std::function<void(const char*)> hook;
    return hook;
}

inline void ReportGpuFailure(const char* where)
{
    if (const auto& hook = GpuFailureHook()) hook(where);
}
}
