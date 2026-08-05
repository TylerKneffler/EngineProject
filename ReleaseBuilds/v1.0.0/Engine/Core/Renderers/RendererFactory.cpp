#include "pch.h"
#include "RendererFactory.h"
#include "DX12/DX12EditorRenderer.h"
#include "DX12/DX12GameRenderer.h"
#include "DX11/DX11EditorRenderer.h"
#include "DX11/DX11GameRenderer.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "Vulkan/VulkanEditorRenderer.h"
#include "Vulkan/VulkanGameRenderer.h"
#include <volk.h>
#endif
#include "Core/ProjectLoader.h"
#include <stdexcept>
#include <sstream>

namespace
{
std::string NormalizeApi(std::string api)
{
    for (auto& c : api)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (api == "directx" || api == "dx12" || api == "d3d12") return "directx12";
    if (api == "dx11" || api == "d3d11") return "directx11";
    if (api == "vk") return "vulkan";
    return api;
}

std::string HResultText(HRESULT hr)
{
    std::ostringstream out;
    out << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return out.str();
}

RendererOption ProbeDX12()
{
    RendererOption result{ "DirectX12" };
    HMODULE runtime = LoadLibraryW(L"d3d12.dll");
    if (!runtime)
    {
        result.unavailableReason = "The Direct3D 12 Windows runtime (d3d12.dll) is not installed.";
        return result;
    }
    FreeLibrary(runtime);

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    result.available = SUCCEEDED(hr);
    if (!result.available)
        result.unavailableReason = "No Direct3D 12-capable GPU/driver was found (" + HResultText(hr) + ").";
    return result;
}

RendererOption ProbeDX11()
{
    RendererOption result{ "DirectX11" };
    HMODULE runtime = LoadLibraryW(L"d3d11.dll");
    if (!runtime)
    {
        result.unavailableReason = "The Direct3D 11 Windows runtime (d3d11.dll) is not installed.";
        return result;
    }
    FreeLibrary(runtime);

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL selected{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &device, &selected, &context);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION,
            &device, &selected, &context);
    result.available = SUCCEEDED(hr);
    if (!result.available)
        result.unavailableReason = "No Direct3D 11-capable GPU/driver was found (" + HResultText(hr) + ").";
    return result;
}

RendererOption ProbeVulkan()
{
    RendererOption result{ "Vulkan" };
#if !defined(ENGINE_VULKAN_ENABLED)
    result.unavailableReason = "The Vulkan backend was not included in this build (DXC was not found during CMake configuration).";
#else
    if (volkInitialize() != VK_SUCCESS)
    {
        result.unavailableReason = "The Vulkan runtime (vulkan-1.dll) is not installed by the graphics driver.";
        return result;
    }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "EngineProject Probe"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    createInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult created = vkCreateInstance(&createInfo, nullptr, &instance);
    if (created != VK_SUCCESS)
    {
        result.unavailableReason = "The Vulkan loader could not create an instance (" + HResultText(created) + ").";
        return result;
    }
    volkLoadInstance(instance);
    uint32_t count = 0;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    result.available = enumerated == VK_SUCCESS && count > 0;
    if (!result.available)
        result.unavailableReason = "No Vulkan-capable GPU/driver was found.";
    vkDestroyInstance(instance, nullptr);
#endif
    return result;
}
}

// ---------------------------------------------------------------------------
// RendererFactory::CreateEditorRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IEditorRenderer> RendererFactory::CreateEditorRenderer(
    const ProjectSettings& settings)
{
    std::string api = NormalizeApi(settings.editorRenderingAPI);
    std::string unavailableReason;
    if (!IsRendererAvailable(api, &unavailableReason))
        throw std::runtime_error(settings.editorRenderingAPI + " is unavailable: " + unavailableReason);

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12EditorRenderer>();
    }
    if (api == "directx11" || api == "dx11" || api == "d3d11")
    {
        return std::make_unique<DX11EditorRenderer>();
    }
#if defined(ENGINE_VULKAN_ENABLED)
    if (api == "vulkan" || api == "vk")
        return std::make_unique<VulkanEditorRenderer>();
#endif
    
    throw std::runtime_error(
        "Unsupported editor rendering API: " + settings.editorRenderingAPI + 
        "\nSupported: DirectX12, DirectX11, Vulkan");
}

// ---------------------------------------------------------------------------
// RendererFactory::CreateGameRenderer
// ---------------------------------------------------------------------------
std::unique_ptr<IGameRenderer> RendererFactory::CreateGameRenderer(
    const ProjectSettings& settings)
{
    std::string api = NormalizeApi(settings.gameRenderingAPI);
    std::string unavailableReason;
    if (!IsRendererAvailable(api, &unavailableReason))
        throw std::runtime_error(settings.gameRenderingAPI + " is unavailable: " + unavailableReason);

    if (api == "directx12" || api == "directx" || api == "dx12" || api == "d3d12")
    {
        return std::make_unique<DX12GameRenderer>();
    }
    if (api == "directx11" || api == "dx11" || api == "d3d11")
    {
        return std::make_unique<DX11GameRenderer>();
    }
#if defined(ENGINE_VULKAN_ENABLED)
    if (api == "vulkan" || api == "vk")
        return std::make_unique<VulkanGameRenderer>();
#endif
    
    throw std::runtime_error(
        "Unsupported game rendering API: " + settings.gameRenderingAPI + 
        "\nSupported: DirectX12, DirectX11, Vulkan");
}

// ---------------------------------------------------------------------------
// RendererFactory::GetSupportedRenderers
// ---------------------------------------------------------------------------
std::string RendererFactory::GetSupportedRenderers()
{
    return "DirectX12, DirectX11, Vulkan";
}

const std::vector<RendererOption>& RendererFactory::GetRendererOptions()
{
    static const std::vector<RendererOption> options = []
    {
        std::vector<RendererOption> detected{ ProbeDX12(), ProbeDX11(), ProbeVulkan() };
        for (const auto& option : detected)
        {
            std::string message = "[RendererAvailability] " + option.name + ": " +
                (option.available ? "available" : "unavailable - " + option.unavailableReason) + "\n";
            OutputDebugStringA(message.c_str());
        }
        return detected;
    }();
    return options;
}

bool RendererFactory::IsRendererAvailable(const std::string& api, std::string* reason)
{
    const std::string normalized = NormalizeApi(api);
    for (const auto& option : GetRendererOptions())
    {
        if (NormalizeApi(option.name) == normalized)
        {
            if (reason) *reason = option.unavailableReason;
            return option.available;
        }
    }
    if (reason) *reason = "This renderer is not included in the current engine build.";
    return false;
}
