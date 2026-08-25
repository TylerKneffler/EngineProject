#pragma once
#include "Core/Graphics/IShader.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <string>

namespace Engine::Renderers
{
// ---------------------------------------------------------------------------
// D3D12Shader — DirectX 12 shader bytecode wrapper
// ---------------------------------------------------------------------------
class D3D12Shader : public Engine::Graphics::IShader
{
public:
    explicit D3D12Shader(Microsoft::WRL::ComPtr<ID3DBlob> bytecode)
        : m_bytecode(bytecode) {}

    const void* GetBytecode() const override
    {
        return m_bytecode ? m_bytecode->GetBufferPointer() : nullptr;
    }

    size_t GetBytecodeSize() const override
    {
        return m_bytecode ? m_bytecode->GetBufferSize() : 0;
    }

private:
    Microsoft::WRL::ComPtr<ID3DBlob> m_bytecode;
};

// ---------------------------------------------------------------------------
// D3D12ShaderCompiler — DirectX 12 shader compilation
// ---------------------------------------------------------------------------
class D3D12ShaderCompiler : public Engine::Graphics::IShaderCompiler
{
public:
    std::unique_ptr<Engine::Graphics::IShader> CompileFromFile(
        const std::string& filePath,
        const char* entryPoint,
        CompileProfile profile) override;

    std::string GetLastError() const override { return m_lastError; }

private:
    mutable std::string m_lastError;
};
}
