#include "pch.h"
#include "ImGuiUiBackend.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "imgui_impl_vulkan.h"
#endif
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/DX12/DX12EditorRenderer.h"
#include "Engine/Editor/UI/ImGui/EditorUI.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanEditorRenderer.h"
#endif

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
#if defined(ENGINE_VULKAN_ENABLED)
PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* instance)
{
    return vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(instance), name);
}

int CreateImGuiWin32VulkanSurface(ImGuiViewport* viewport, ImU64 instance,
    const void* allocator, ImU64* outSurface)
{
    VkWin32SurfaceCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    createInfo.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    createInfo.hinstance = GetModuleHandle(nullptr);
    return static_cast<int>(vkCreateWin32SurfaceKHR(
        reinterpret_cast<VkInstance>(instance), &createInfo,
        static_cast<const VkAllocationCallbacks*>(allocator),
        reinterpret_cast<VkSurfaceKHR*>(outSurface)));
}
#endif
}

ImGuiUiBackend::ImGuiUiBackend() = default;

ImGuiUiBackend::~ImGuiUiBackend()
{
    Shutdown();
}

bool ImGuiUiBackend::Initialize(void* nativeWindow, IEditorRenderer& renderer)
{
    Shutdown();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    HWND window = static_cast<HWND>(nativeWindow);
    if (!ImGui_ImplWin32_Init(window))
    {
        Shutdown();
        return false;
    }

    if (auto* dx11 = dynamic_cast<DX11EditorRenderer*>(&renderer))
    {
        if (!ImGui_ImplDX11_Init(dx11->GetDevice(), dx11->GetDeviceContext()))
        {
            Shutdown();
            return false;
        }
        m_graphicsApi = GraphicsApi::DirectX11;
    }
    else if (auto* dx12 = dynamic_cast<DX12EditorRenderer*>(&renderer))
    {
        ImGui_ImplDX12_InitInfo info{};
        info.Device = dx12->GetDevice();
        info.CommandQueue = dx12->GetCommandQueue();
        info.NumFramesInFlight = static_cast<int>(DX12EditorRenderer::FRAME_COUNT);
        info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = dx12->GetUiDescriptorHeap();
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* initInfo,
            D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            *cpu = initInfo->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
            *gpu = initInfo->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
            D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
        if (!ImGui_ImplDX12_Init(&info))
        {
            Shutdown();
            return false;
        }
        m_graphicsApi = GraphicsApi::DirectX12;
    }
#if defined(ENGINE_VULKAN_ENABLED)
    else if (auto* vulkan = dynamic_cast<VulkanEditorRenderer*>(&renderer))
    {
        auto& core = vulkan->GetRenderCore();
        ImGui::GetPlatformIO().Platform_CreateVkSurface = CreateImGuiWin32VulkanSurface;
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, LoadVulkanFunction,
            reinterpret_cast<void*>(core.GetInstance())))
        {
            Shutdown();
            return false;
        }
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1;
        info.Instance = core.GetInstance();
        info.PhysicalDevice = core.GetPhysicalDevice();
        info.Device = core.GetDevice();
        info.QueueFamily = core.GetQueueFamily();
        info.Queue = core.GetQueue();
        info.DescriptorPoolSize = 1024;
        info.MinImageCount = core.GetMinImageCount();
        info.ImageCount = core.GetImageCount();
        info.PipelineInfoMain.RenderPass = core.GetMainRenderPass();
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&info))
        {
            Shutdown();
            return false;
        }
        renderer.SetUiTextureHooks({
            [](void* sampler, void* imageView, uint32_t layout) -> void*
            {
                VkDescriptorSet descriptor = ImGui_ImplVulkan_AddTexture(
                    reinterpret_cast<VkSampler>(sampler),
                    reinterpret_cast<VkImageView>(imageView),
                    static_cast<VkImageLayout>(layout));
                return reinterpret_cast<void*>(descriptor);
            },
            [](void* texture)
            {
                ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(texture));
            }
        });
        m_graphicsApi = GraphicsApi::Vulkan;
    }
#endif
    else
    {
        Shutdown();
        return false;
    }

    m_renderer = &renderer;
    m_initialized = true;
    return true;
}

void ImGuiUiBackend::Shutdown()
{
    m_presentation.reset();
    if (ImGui::GetCurrentContext())
    {
        if (m_graphicsApi == GraphicsApi::DirectX11 && ImGui::GetIO().BackendRendererUserData)
            ImGui_ImplDX11_Shutdown();
        else if (m_graphicsApi == GraphicsApi::DirectX12 && ImGui::GetIO().BackendRendererUserData)
            ImGui_ImplDX12_Shutdown();
#if defined(ENGINE_VULKAN_ENABLED)
        else if (m_graphicsApi == GraphicsApi::Vulkan && ImGui::GetIO().BackendRendererUserData)
            ImGui_ImplVulkan_Shutdown();
#endif
        if (ImGui::GetIO().BackendPlatformUserData)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    m_graphicsApi = GraphicsApi::None;
    m_renderer = nullptr;
    m_initialized = false;
}

void ImGuiUiBackend::DrawEditor(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    DrawEditorPresentation(state, playState, buildManager);
}

void ImGuiUiBackend::DrawEditorPresentation(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    if (!m_presentation)
        m_presentation = std::make_unique<EditorUI>(&state);
    m_presentation->SetGameBuildManager(buildManager);
    m_presentation->Render(playState);
}

bool ImGuiUiBackend::HandleMessage(void* nativeWindow, uint32_t message,
    uintptr_t wParam, intptr_t lParam)
{
    return m_initialized && ImGui_ImplWin32_WndProcHandler(
        static_cast<HWND>(nativeWindow), message,
        static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam)) != 0;
}

void ImGuiUiBackend::Resize(uint32_t, uint32_t) {}

void ImGuiUiBackend::BeginFrame()
{
    if (!m_initialized) return;
    if (m_graphicsApi == GraphicsApi::DirectX11) ImGui_ImplDX11_NewFrame();
    else if (m_graphicsApi == GraphicsApi::DirectX12) ImGui_ImplDX12_NewFrame();
#if defined(ENGINE_VULKAN_ENABLED)
    else if (m_graphicsApi == GraphicsApi::Vulkan) ImGui_ImplVulkan_NewFrame();
#endif
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiUiBackend::Render(void* commandBuffer)
{
    if (!m_initialized) return;
    ImGui::Render();
    RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void ImGuiUiBackend::RenderDrawData(ImDrawData* drawData, void* commandBuffer)
{
    if (!m_initialized || !drawData) return;
    if (m_graphicsApi == GraphicsApi::DirectX11)
        ImGui_ImplDX11_RenderDrawData(drawData);
    else if (m_graphicsApi == GraphicsApi::DirectX12)
    {
        auto* dx12 = static_cast<DX12EditorRenderer*>(m_renderer);
        ID3D12DescriptorHeap* heaps[] = { dx12->GetUiDescriptorHeap() };
        auto* commands = static_cast<ID3D12GraphicsCommandList*>(commandBuffer);
        commands->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(drawData, commands);
    }
#if defined(ENGINE_VULKAN_ENABLED)
    else if (m_graphicsApi == GraphicsApi::Vulkan)
        ImGui_ImplVulkan_RenderDrawData(drawData,
            reinterpret_cast<VkCommandBuffer>(commandBuffer));
#endif
}

void ImGuiUiBackend::EndHiddenFrame()
{
    if (m_initialized)
    {
        ImGui::EndFrame();
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            ImGui::UpdatePlatformWindows();
    }
}

void ImGuiUiBackend::EndFrame()
{
    if (!m_initialized) return;
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}
