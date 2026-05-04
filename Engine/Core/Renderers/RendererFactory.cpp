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
    std::string api = settings.editorRenderingAPI;
    
    // Normalize API name (case-insensitive)
    for (auto& c : api) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12EditorRenderer>();
    }
    
    throw std::runtime_error(
        "Unsupported editor rendering API: " + settings.editorRenderingAPI + 
        "\nSupported: DirectX12");
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
    
    throw std::runtime_error(
        "Unsupported game rendering API: " + settings.gameRenderingAPI + 
        "\nSupported: DirectX12");
}

// ---------------------------------------------------------------------------
// RendererFactory::GetSupportedRenderers
// ---------------------------------------------------------------------------
std::string RendererFactory::GetSupportedRenderers()
{
    return "DirectX12";
}
