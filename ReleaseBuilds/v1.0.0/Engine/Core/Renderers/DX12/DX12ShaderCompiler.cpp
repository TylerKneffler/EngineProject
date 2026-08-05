#include "DX12ShaderCompiler.h"
#include <d3dcompiler.h>
#include <sstream>

std::unique_ptr<IShader> D3D12ShaderCompiler::CompileFromFile(
    const std::string& filePath,
    const char* entryPoint,
    CompileProfile profile)
{
    m_lastError.clear();

    // Convert profile enum to HLSL target string
    const char* target = nullptr;
    switch (profile)
    {
        case CompileProfile::VS_5_0: target = "vs_5_0"; break;
        case CompileProfile::PS_5_0: target = "ps_5_0"; break;
        case CompileProfile::CS_5_0: target = "cs_5_0"; break;
        default:
            m_lastError = "Unknown compile profile";
            return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    // Convert string path to LPCWSTR
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, &wpath[0], wlen);

    HRESULT hr = D3DCompileFromFile(
        wpath.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        target,
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0,
        &bytecode,
        &errors);

    if (FAILED(hr))
    {
        if (errors)
        {
            m_lastError = std::string(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        else
        {
            std::ostringstream oss;
            oss << "D3DCompileFromFile failed with HRESULT 0x" << std::hex << hr;
            m_lastError = oss.str();
        }
        return nullptr;
    }

    return std::make_unique<D3D12Shader>(bytecode);
}
