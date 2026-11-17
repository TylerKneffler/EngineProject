// D3DShaderCache: hits reuse bytecode, editing an #included file invalidates
// the entry, and failed compiles are never stored.
#include "Core/Renderers/D3DShaderCache.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Engine::Renderers::D3DShaderCache;

namespace
{
void Require(bool value, const char* what)
{
    if (value) return;
    std::fprintf(stderr, "D3DShaderCacheTests failed: %s\n", what);
    std::abort();
}

void Write(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

size_t CachedFiles(const fs::path& directory)
{
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().extension() == ".dxbc") ++count;
    return count;
}

bool Same(ID3DBlob* a, ID3DBlob* b)
{
    return a && b && a->GetBufferSize() == b->GetBufferSize() &&
        std::memcmp(a->GetBufferPointer(), b->GetBufferPointer(), a->GetBufferSize()) == 0;
}
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "EngineShaderCacheTest";
    fs::remove_all(root);
    const fs::path cache = root / "cache";
    const fs::path sources = root / "src";
    fs::create_directories(cache);
    fs::create_directories(sources / "inc");
    // Must be set before the first cache use (the directory is read once).
    SetEnvironmentVariableA("ENGINE_SHADER_CACHE_DIR", cache.string().c_str());
    SetEnvironmentVariableA("ENGINE_SHADER_CACHE", "1");

    const fs::path shader = sources / "main.hlsl";
    const fs::path include = sources / "inc" / "color.hlsli";
    Write(shader, "#include \"inc/color.hlsli\"\n"
        "float4 PSMain() : SV_Target { return Color(); }\n");
    Write(include, "float4 Color() { return float4(1, 0, 0, 1); }\n");

    std::string error;
    auto first = D3DShaderCache::CompileFromFile(shader.string(), "PSMain", "ps_5_0", 0, error);
    Require(first && error.empty(), "initial compile");
    Require(CachedFiles(cache) == 1, "miss stores one entry");

    auto second = D3DShaderCache::CompileFromFile(shader.string(), "PSMain", "ps_5_0", 0, error);
    Require(Same(first.Get(), second.Get()), "hit returns identical bytecode");
    Require(CachedFiles(cache) == 1, "hit adds no entry");

    // Changing only the included file must produce a new entry and new code.
    Write(include, "float4 Color() { return float4(0, 1, 0, 1); }\n");
    auto edited = D3DShaderCache::CompileFromFile(shader.string(), "PSMain", "ps_5_0", 0, error);
    Require(edited && error.empty(), "compile after include edit");
    Require(!Same(first.Get(), edited.Get()), "include edit changes bytecode");
    Require(CachedFiles(cache) == 2, "include edit stores a new entry");

    // Different entry point or flags are separate entries.
    D3DShaderCache::CompileFromFile(shader.string(), "PSMain", "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, error);
    Require(CachedFiles(cache) == 3, "flags are part of the key");

    // Compile errors are reported and not cached.
    Write(include, "float4 Color() { return undefined_symbol; }\n");
    auto failed = D3DShaderCache::CompileFromFile(shader.string(), "PSMain", "ps_5_0", 0, error);
    Require(!failed && !error.empty(), "error reported");
    Require(CachedFiles(cache) == 3, "failed compile not stored");

    fs::remove_all(root);
    std::printf("D3DShaderCacheTests passed\n");
    return 0;
}
