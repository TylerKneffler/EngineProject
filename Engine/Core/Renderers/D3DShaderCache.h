#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>

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
//
// Every request (working directory, path, entry point, target, flags) is
// listed once in requests.txt there, so a build step can compile the same
// shaders ahead of the next launch (Engine/Tools/ShaderPrecompile.cpp): the
// key includes the preprocessed text, which names files as the request did.
//
// Lower optimization does not pay for iteration: compiling every recorded
// variant in parallel took 261 s at level 3, 307 s at level 1 and 191 s
// unoptimized (2026-11-25; Object.hlsl's pixel shaders dominate).
class D3DShaderCache
{
public:
    // Returns the bytecode, or null with `error` set.
    static Microsoft::WRL::ComPtr<ID3DBlob> CompileFromFile(
        const std::string& filePath, const char* entryPoint, const char* target,
        UINT flags, std::string& error);

    struct Request
    {
        std::string directory, filePath, entryPoint, target;
        UINT flags = 0;
    };
    // The requests listed so far (requests.txt).
    static std::vector<Request> RecordedRequests();
    // The precompile tool replays requests without listing them again.
    static void SetRecordRequests(bool record);
    static std::string CacheDirectoryPath();
};
}
