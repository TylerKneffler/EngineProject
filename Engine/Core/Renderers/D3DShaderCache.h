#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>

namespace Engine::Renderers
{
// Persistent bytecode cache for runtime HLSL compiles (DX11 and DX12).
//
// The key hashes the fully preprocessed source (every #include resolved),
// entry point, target, compile flags and the d3dcompiler version, so editing
// any included file invalidates exactly the shaders that use it. A miss
// compiles with D3DCompileFromFile exactly as before and stores the result;
// failed compiles are never stored.
//
// Location: %TEMP%/EngineShaderCache, or ENGINE_SHADER_CACHE_DIR.
// ENGINE_SHADER_CACHE=0 disables it.
class D3DShaderCache
{
public:
    // Returns the bytecode, or null with `error` set.
    static Microsoft::WRL::ComPtr<ID3DBlob> CompileFromFile(
        const std::string& filePath, const char* entryPoint, const char* target,
        UINT flags, std::string& error);
};
}
