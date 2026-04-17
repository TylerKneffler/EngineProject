#include "pch.h"
#include "RendererFactory.h"
#include "DX12/DX12EditorRenderer.h"
#include "DX12/DX12GameRenderer.h"
#include "Core/ProjectLoader.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// RendererFactory::CreateEditorRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IEditorRenderer> RendererFactory::CreateEditorRenderer(
    const ProjectSettings& settings)
{
    std::string api = settings.renderingAPI;
    
    // Normalize API name (case-insensitive)
    for (auto& c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12EditorRenderer>();
    }
    // Future renderers can be added here:
    // else if (api == "vulkan")
    // {
    //     return std::make_unique<VulkanEditorRenderer>();
    // }
    
    throw std::runtime_error(
        "Unsupported rendering API: " + settings.renderingAPI + 
        "\nSupported: DirectX12, Vulkan (coming soon)");
}

// ---------------------------------------------------------------------------
// RendererFactory::CreateGameRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IGameRenderer> RendererFactory::CreateGameRenderer(
    const ProjectSettings& settings)
{
    std::string api = settings.renderingAPI;
    
    // Normalize API name (case-insensitive)
    for (auto& c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12GameRenderer>();
    }
    // Future renderers can be added here:
    // else if (api == "vulkan")
    // {
    //     return std::make_unique<VulkanGameRenderer>();
    // }
    
    throw std::runtime_error(
        "Unsupported rendering API: " + settings.renderingAPI + 
        "\nSupported: DirectX12, Vulkan (coming soon)");
}

// ---------------------------------------------------------------------------
// RendererFactory::GetSupportedRenderers
// ---------------------------------------------------------------------------
std::string RendererFactory::GetSupportedRenderers()
{
    return "DirectX12 (current), Vulkan (in development)";
}
