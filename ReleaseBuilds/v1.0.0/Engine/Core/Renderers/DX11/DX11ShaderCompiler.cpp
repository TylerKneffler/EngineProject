#include "pch.h"
#include "DX11ShaderCompiler.h"
#include <sstream>

std::unique_ptr<IShader> D3D11ShaderCompiler::CompileFromFile(
    const std::string& filePath,
    const char* entryPoint,
    CompileProfile profile)
{
    m_lastError.clear();

    const char* target = nullptr;
    switch (profile)
    {
        case CompileProfile::VS_5_0: target = "vs_5_0"; break;
        case CompileProfile::PS_5_0: target = "ps_5_0"; break;
        case CompileProfile::CS_5_0: target = "cs_5_0"; break;
        default: m_lastError = "Unknown shader profile"; return nullptr;
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
    if (length <= 1)
    {
        m_lastError = "Invalid shader path";
        return nullptr;
    }
    std::wstring widePath(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, widePath.data(), length);
    widePath.pop_back();

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS
#if defined(_DEBUG)
        | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION
#endif
        ;

    HRESULT hr = D3DCompileFromFile(
        widePath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, flags, 0, &bytecode, &errors);
    if (FAILED(hr))
    {
        if (errors)
            m_lastError.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        else
        {
            std::ostringstream out;
            out << "D3DCompileFromFile failed with HRESULT 0x" << std::hex << hr;
            m_lastError = out.str();
        }
        return nullptr;
    }

    return std::make_unique<D3D11Shader>(std::move(bytecode));
}
