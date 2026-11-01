#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanShaderCompiler.h"
#include <filesystem>
#include <fstream>

#ifndef ENGINE_VULKAN_SHADER_PATH
#define ENGINE_VULKAN_SHADER_PATH "VulkanShaders/"
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif
#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Renderers
{
std::unique_ptr<Engine::Graphics::IShader> VulkanShaderCompiler::CompileFromFile(
    const std::string& filePath, const char* entryPoint, CompileProfile profile)
{
    m_lastError.clear();
    std::string name = std::filesystem::path(filePath).stem().string();
    const char* stage = profile == CompileProfile::VS_5_0 ? "VS" :
                        profile == CompileProfile::PS_5_0 ? "PS" : "CS";
    const char* defaultEntry = profile == CompileProfile::VS_5_0
        ? "VSMain" : (profile == CompileProfile::PS_5_0 ? "PSMain" : "CSMain");
    const std::string entrySuffix = entryPoint &&
        std::string(entryPoint) != defaultEntry
        ? "." + std::string(entryPoint) : std::string{};
    const std::string binaryName = name + entrySuffix + "." + stage + ".spv";
    // Material assets are keyed by their full relative path, not just their
    // filename. Two folders may legitimately contain different Tint.hlsl files.
    namespace fs = std::filesystem;
    fs::path relativeBinary = binaryName;
    const fs::path source = fs::absolute(filePath).lexically_normal();
    const auto engineRelative = source.lexically_relative(
        fs::absolute(ENGINE_ASSETS_PATH).lexically_normal());
    const auto projectRelative = source.lexically_relative(
        fs::absolute("Assets").lexically_normal());
    const auto inRoot = [](const fs::path& relative)
    { return !relative.empty() && *relative.begin() != ".."; };
    if (inRoot(engineRelative))
        relativeBinary = fs::path("Materials/Engine") /
            (engineRelative.generic_string() + entrySuffix + "." + stage + ".spv");
    else if (inRoot(projectRelative))
        relativeBinary = fs::path("Materials/Project") /
            (projectRelative.generic_string() + entrySuffix + "." + stage + ".spv");
    else
    {
        // Arbitrary paths outside Assets use adjacent precompiled binaries.
        const auto shaderRelative = source.lexically_relative(
            fs::absolute(ENGINE_SHADERS_PATH).lexically_normal());
        if (!inRoot(shaderRelative))
            relativeBinary = source.string() + entrySuffix + "." + stage + ".spv";
    }
    fs::path binary = fs::path("VulkanShaders") / relativeBinary;
    if (!std::filesystem::is_regular_file(binary))
        binary = fs::path(ENGINE_VULKAN_SHADER_PATH) / relativeBinary;
    const fs::path sidecar = filePath + entrySuffix + "." + stage + ".spv";
    if (fs::is_regular_file(sidecar)) binary = sidecar;
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
}
#endif
