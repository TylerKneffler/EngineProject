#include "DX12ShaderCompiler.h"
#include "Core/Renderers/D3DShaderCache.h"
#include <d3dcompiler.h>
#include <cstdlib>
#include <sstream>

namespace Engine::Renderers
{
std::unique_ptr<Engine::Graphics::IShader> D3D12ShaderCompiler::CompileFromFile(
    const std::string& filePath,
    const char* entryPoint,
    CompileProfile profile)
{
    m_lastError.clear();

    // Convert profile enum to HLSL target string. DX12 compiles shader model
    // 6 with DXC (D3DShaderCache::DxcAvailable; ENGINE_SHADER_DXC=0 keeps
    // fxc's 5.0 for A/B and for a machine without dxcompiler.dll).
    static const bool dxc = [] {
        const char* value = std::getenv("ENGINE_SHADER_DXC");
        const bool use = !(value && value[0] == '0') && D3DShaderCache::DxcAvailable();
        if (use) D3DShaderCache::PreferShaderModel6();
        return use;
    }();
    const char* target = nullptr;
    switch (profile)
    {
        case CompileProfile::VS_5_0: target = dxc ? "vs_6_0" : "vs_5_0"; break;
        case CompileProfile::PS_5_0: target = dxc ? "ps_6_0" : "ps_5_0"; break;
        case CompileProfile::CS_5_0: target = dxc ? "cs_6_0" : "cs_5_0"; break;
        default:
            m_lastError = "Unknown compile profile";
            return nullptr;
    }

    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG
#if defined(ENGINE_SHADER_UNOPTIMIZED_DEBUG)
        | D3DCOMPILE_SKIP_OPTIMIZATION
#else
        | D3DCOMPILE_OPTIMIZATION_LEVEL3
#endif
        ;
    // Persistent bytecode cache; a miss compiles exactly as before.
    auto bytecode = D3DShaderCache::CompileFromFile(filePath, entryPoint,
        target, flags, m_lastError);
    if (!bytecode)
        return nullptr;
    return std::make_unique<D3D12Shader>(std::move(bytecode));
}
}
