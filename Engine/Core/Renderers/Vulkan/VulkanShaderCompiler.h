#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IShader.h"
#include <vector>

class VulkanShader : public IShader
{
public:
    explicit VulkanShader(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}
    const void* GetBytecode() const override { return m_bytes.data(); }
    size_t GetBytecodeSize() const override { return m_bytes.size(); }
private:
    std::vector<uint8_t> m_bytes;
};

class VulkanShaderCompiler : public IShaderCompiler
{
public:
    std::unique_ptr<IShader> CompileFromFile(const std::string& filePath,
        const char* entryPoint, CompileProfile profile) override;
    std::string GetLastError() const override { return m_lastError; }
private:
    std::string m_lastError;
};
#endif
