#include "pch.h"
#include "RendererFactory.h"
#include "DX12/DX12EditorRenderer.h"
#include "DX12/DX12GameRenderer.h"
#include "Vulkan/VulkanEditorRenderer.h"
#include "Vulkan/VulkanGameRenderer.h"
#include "Core/ProjectLoader.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// RendererFactory::CreateEditorRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IEditorRenderer> RendererFactory::CreateEditorRenderer(
    const ProjectSettings& settings)
{
    std::string api = settings.editorRenderingAPI;
    
    // Normalize API name (case-insensitive)
    for (auto& c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12EditorRenderer>();
    }
    else if (api == "vulkan")
    {
#if defined(ENGINE_VULKAN_ENABLED)
        return std::make_unique<VulkanEditorRenderer>();
#else
        throw std::runtime_error(
            "Vulkan editor renderer requested, but Vulkan SDK support is not enabled in this build.");
#endif
    }
    
    throw std::runtime_error(
        "Unsupported editor rendering API: " + settings.editorRenderingAPI + 
        "\nSupported: DirectX12, Vulkan");
}

// ---------------------------------------------------------------------------
// RendererFactory::CreateGameRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IGameRenderer> RendererFactory::CreateGameRenderer(
    const ProjectSettings& settings)
{
    std::string api = settings.gameRenderingAPI;
    
    // Normalize API name (case-insensitive)
    for (auto& c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12GameRenderer>();
    }
    else if (api == "vulkan")
    {
#if defined(ENGINE_VULKAN_ENABLED)
        return std::make_unique<VulkanGameRenderer>();
#else
        throw std::runtime_error(
            "Vulkan game renderer requested, but Vulkan SDK support is not enabled in this build.");
#endif
    }
    
    throw std::runtime_error(
        "Unsupported game rendering API: " + settings.gameRenderingAPI + 
        "\nSupported: DirectX12, Vulkan");
}

// ---------------------------------------------------------------------------
// RendererFactory::GetSupportedRenderers
// ---------------------------------------------------------------------------
std::string RendererFactory::GetSupportedRenderers()
{
    return "DirectX12 (current), Vulkan (in development)";
}
