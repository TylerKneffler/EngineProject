#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanShaderCompiler.h"
#include <filesystem>
#include <fstream>

#ifndef ENGINE_VULKAN_SHADER_PATH
#define ENGINE_VULKAN_SHADER_PATH "VulkanShaders/"
#endif

std::unique_ptr<IShader> VulkanShaderCompiler::CompileFromFile(
    const std::string& filePath, const char*, CompileProfile profile)
{
    m_lastError.clear();
    std::string name = std::filesystem::path(filePath).stem().string();
    const char* stage = profile == CompileProfile::VS_5_0 ? "VS" :
                        profile == CompileProfile::PS_5_0 ? "PS" : "CS";
    const std::string binaryName = name + "." + stage + ".spv";
    std::filesystem::path binary = std::filesystem::path("VulkanShaders") / binaryName;
    if (!std::filesystem::is_regular_file(binary))
        binary = std::filesystem::path(ENGINE_VULKAN_SHADER_PATH) / binaryName;
    std::ifstream stream(binary, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        m_lastError = "Vulkan shader binary not found: " + binary.string();
        return nullptr;
    }
    const auto size = stream.tellg();
    if (size <= 0 || (size % 4) != 0)
    {
        m_lastError = "Invalid SPIR-V binary: " + binary.string();
        return nullptr;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return std::make_unique<VulkanShader>(std::move(bytes));
}
#endif
