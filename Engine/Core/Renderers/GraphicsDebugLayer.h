#pragma once

#include <cstdlib>

namespace Engine::Renderers
{
// Graphics API validation: D3D11/D3D12 debug layers, the DXGI debug factory,
// DRED breadcrumbs and per-buffer address logging. These can halve the frame
// rate, so they are decoupled from the C++ build configuration:
//
//   default            on in Debug builds, off in Release/RelWithDebInfo
//   ENGINE_GPU_DEBUG=1 force on  (e.g. chasing a GPU bug in an optimized build)
//   ENGINE_GPU_DEBUG=0 force off (e.g. profiling a Debug build)
//
// Set the variable in the shell or in the debugger's environment before launch;
// the value is read once because the layers must be chosen before any device
// or factory exists.
inline bool IsGraphicsDebugLayerEnabled()
{
    static const bool enabled = []
    {
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "ENGINE_GPU_DEBUG") == 0 && value)
        {
            const bool on = value[0] != '\0' && value[0] != '0';
            std::free(value);
            return on;
        }
#if defined(_DEBUG)
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}
}
