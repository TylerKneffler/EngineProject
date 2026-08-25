#pragma once
#include "Core/Graphics/IShader.h"
#include <wrl/client.h>
#include <d3dcompiler.h>

namespace Engine::Renderers
{
class D3D11Shader : public Engine::Graphics::IShader
{
public:
    explicit D3D11Shader(Microsoft::WRL::ComPtr<ID3DBlob> bytecode)
        : m_bytecode(std::move(bytecode)) {}

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

class D3D11ShaderCompiler : public Engine::Graphics::IShaderCompiler
{
public:
    using CompileProfile = Engine::Graphics::IShaderCompiler::CompileProfile;

    std::unique_ptr<Engine::Graphics::IShader> CompileFromFile(
        const std::string& filePath,
        const char* entryPoint,
        CompileProfile profile) override;

    std::string GetLastError() const override { return m_lastError; }

private:
    std::string m_lastError;
};
}
