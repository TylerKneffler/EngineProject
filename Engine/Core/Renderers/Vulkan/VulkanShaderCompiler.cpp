#include "pch.h"
#include "VulkanShaderCompiler.h"
#include <fstream>

std::unique_ptr<IShader> VulkanShaderCompiler::CompileFromFile(
    const std::string& filePath,
    const char*        entryPoint,
    CompileProfile     /*profile*/)
{
    // Build per-entry-point SPIR-V path: "Grid.hlsl.VSMain.spv"
    // This matches the filenames produced by the CMake dxc compile step.
    std::string ep = (entryPoint && *entryPoint) ? entryPoint : "main";
    std::string spvPath;
    if (filePath.size() >= 4 &&
        filePath.compare(filePath.size() - 4, 4, ".spv") == 0)
    {
        spvPath = filePath;  // already a .spv path
    }
    else
    {
        spvPath = filePath + "." + ep + ".spv";
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        m_lastError = "Failed to open SPIR-V file: " + spvPath;
        return nullptr;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0)
    {
        m_lastError = "SPIR-V file is empty: " + spvPath;
        return nullptr;
    }

    std::vector<uint8_t> bytecode(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytecode.data()), size);
    if (!file)
    {
        m_lastError = "Failed to read SPIR-V file: " + spvPath;
        return nullptr;
    }

    m_lastError.clear();
    return std::make_unique<VulkanShader>(std::move(bytecode), ep);
}
