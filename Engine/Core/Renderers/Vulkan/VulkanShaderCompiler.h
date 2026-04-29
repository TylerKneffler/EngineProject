#pragma once
#include "Core/Graphics/IShader.h"
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// VulkanShader — SPIR-V bytecode wrapper
// ---------------------------------------------------------------------------
class VulkanShader : public IShader
{
public:
    VulkanShader(std::vector<uint8_t> bytecode, std::string entryPoint)
        : m_bytecode(std::move(bytecode)), m_entryPoint(std::move(entryPoint)) {}

    const void*        GetBytecode()     const override { return m_bytecode.data(); }
    size_t             GetBytecodeSize() const override { return m_bytecode.size(); }
    const std::string& GetEntryPoint()   const          { return m_entryPoint; }

private:
    std::vector<uint8_t> m_bytecode;
    std::string          m_entryPoint;
};

// ---------------------------------------------------------------------------
// VulkanShaderCompiler — Loads pre-compiled SPIR-V (.spv) shader files
//
// Conventions:
//   - If filePath already ends with ".spv", it is loaded directly.
//   - Otherwise ".spv" is appended to the path.
//   - entryPoint and profile are recorded but not used for loading.
// ---------------------------------------------------------------------------
class VulkanShaderCompiler : public IShaderCompiler
{
public:
    std::unique_ptr<IShader> CompileFromFile(
        const std::string& filePath,
        const char*        entryPoint,
        CompileProfile     profile) override;

    std::string GetLastError() const override { return m_lastError; }

private:
    mutable std::string m_lastError;
};
