#pragma once

// Windows
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

//IMGUI
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

// DirectX 12
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

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
