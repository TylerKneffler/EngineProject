#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanEditorRenderer.h"
#include "VulkanView.h"
#include "imgui_impl_vulkan.h"
#include <algorithm>

namespace
{
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
}

PFN_vkVoidFunction VulkanEditorRenderer::LoadFunction(const char* name, void* instance)
{ return vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(instance), name); }

VulkanEditorRenderer::~VulkanEditorRenderer()
{
    if (m_imguiInitialized)
    {
        m_core.WaitIdle();
        ImGui_ImplVulkan_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    }
}

bool VulkanEditorRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    try
    {
        m_width = width; m_height = height;
        if (!m_core.Init(static_cast<HWND>(hwnd), width, height, false)) return false;
        m_viewContext = { m_core.GetPhysicalDevice(), m_core.GetDevice(), m_core.GetOffscreenRenderPass() };
        m_provider = std::make_unique<VulkanGraphicsProvider>(
            m_core.GetPhysicalDevice(), m_core.GetDevice(), m_core.GetOffscreenRenderPass());
        for (uint32_t i = MAX_SRV_SLOTS - 1; i > 0; --i) m_freeSlots.push_back(i);
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
        if (!ImGui_ImplWin32_Init(hwnd)) throw std::runtime_error("ImGui Win32 initialization failed");
        ImGui::GetPlatformIO().Platform_CreateVkSurface = CreateImGuiWin32VulkanSurface;
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, LoadFunction,
            reinterpret_cast<void*>(m_core.GetInstance())))
            throw std::runtime_error("ImGui Vulkan function loading failed");
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1; info.Instance = m_core.GetInstance();
        info.PhysicalDevice = m_core.GetPhysicalDevice(); info.Device = m_core.GetDevice();
        info.QueueFamily = m_core.GetQueueFamily(); info.Queue = m_core.GetQueue();
        info.DescriptorPoolSize = 1024; info.MinImageCount = m_core.GetMinImageCount();
        info.ImageCount = m_core.GetImageCount(); info.PipelineInfoMain.RenderPass = m_core.GetMainRenderPass();
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui Vulkan initialization failed");
        m_imguiInitialized = true;
        return true;
    }
    catch (const std::exception& error)
    {
        if (ImGui::GetCurrentContext())
        {
            if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplVulkan_Shutdown();
            if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        OutputDebugStringA((std::string("[VulkanEditorRenderer] ") + error.what() + "\n").c_str());
        return false;
    }
}

void VulkanEditorRenderer::Resize(uint32_t width, uint32_t height)
{ if (width && height) { m_width = width; m_height = height; m_core.Resize(width, height); m_dirty = true; } }
void VulkanEditorRenderer::Clear(float r, float g, float b, float a)
{ m_clearColor[0] = r; m_clearColor[1] = g; m_clearColor[2] = b; m_clearColor[3] = a; }

void VulkanEditorRenderer::RenderIfNeeded(std::function<void()> drawFn)
{
    if (!m_dirty) return;
    m_dirty = false; m_commandBuffer = m_core.BeginFrame();
    if (!m_commandBuffer) { m_dirty = true; return; }
    ImGui_ImplVulkan_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if (drawFn) drawFn();
    ImGui::Render();
    VkClearValue clear{}; clear.color = { { m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3] } };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = m_core.GetMainRenderPass(); begin.framebuffer = m_core.GetCurrentFramebuffer();
    begin.renderArea.extent = m_core.GetExtent(); begin.clearValueCount = 1; begin.pClearValues = &clear;
    vkCmdBeginRenderPass(m_commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_commandBuffer);
    vkCmdEndRenderPass(m_commandBuffer);
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    { ImGui::UpdatePlatformWindows(); ImGui::RenderPlatformWindowsDefault(); }
    m_core.EndFrame(); m_commandBuffer = VK_NULL_HANDLE;
}

std::pair<std::pair<void*, void*>, uint32_t> VulkanEditorRenderer::AllocateSrvSlot()
{
    if (m_freeSlots.empty()) throw std::runtime_error("No available Vulkan editor view slots");
    uint32_t slot = m_freeSlots.back(); m_freeSlots.pop_back(); return { { nullptr, nullptr }, slot };
}
void VulkanEditorRenderer::FreeSrvSlot(uint32_t slot)
{
    if (slot && slot < MAX_SRV_SLOTS && std::find(m_freeSlots.begin(), m_freeSlots.end(), slot) == m_freeSlots.end())
        m_freeSlots.push_back(slot);
}
std::unique_ptr<IView> VulkanEditorRenderer::CreateViewBackend() { return std::make_unique<VulkanView>(); }
#endif
