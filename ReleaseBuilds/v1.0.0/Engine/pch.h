#pragma once

// This is a C++ precompiled header. Some editor language services inspect C
// sources with workspace-wide settings, so keep all C++/DirectX declarations
// out of a C translation unit even if the header is injected accidentally.
#ifdef __cplusplus

// Windows
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#if defined(ENGINE_VULKAN_ENABLED)
#include <volk.h>
#endif

// DirectX 12
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

// DirectX 12 helper (from Microsoft DirectX-Headers or d3dx12.h)
// We include a minimal inline helper below instead of requiring an extra dependency.

// C++ Standard Library
#include <wrl/client.h>  // ComPtr
#include <stdexcept>
#include <string>
#include <functional>
#include <cassert>
#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>

// Engine Core
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Script.h"

// Lib links (can also be done in .vcxproj)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw std::runtime_error("HRESULT failed: 0x" +
            std::to_string(static_cast<unsigned long>(hr)));
    }
}

#endif // __cplusplus
