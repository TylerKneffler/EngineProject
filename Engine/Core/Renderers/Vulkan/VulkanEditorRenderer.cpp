#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanEditorRenderer.h"
#include "VulkanView.h"
#include <algorithm>

VulkanEditorRenderer::~VulkanEditorRenderer()
{
    m_core.WaitIdle();
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
        return true;
    }
    catch (const std::exception& error)
    {
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
    if (m_uiHooks.beginFrame) m_uiHooks.beginFrame();
    if (drawFn) drawFn();
    VkClearValue clear{}; clear.color = { { m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3] } };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = m_core.GetMainRenderPass(); begin.framebuffer = m_core.GetCurrentFramebuffer();
    begin.renderArea.extent = m_core.GetExtent(); begin.clearValueCount = 1; begin.pClearValues = &clear;
    vkCmdBeginRenderPass(m_commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    if (m_uiHooks.render) m_uiHooks.render(reinterpret_cast<void*>(m_commandBuffer));
    vkCmdEndRenderPass(m_commandBuffer);
    if (m_uiHooks.endFrame) m_uiHooks.endFrame();
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

void VulkanEditorRenderer::SetUiTextureHooks(EditorUiTextureHooks hooks)
{
    m_viewContext.registerUiTexture = [registerTexture = std::move(hooks.registerTexture)](
        VkSampler sampler, VkImageView view, VkImageLayout layout) -> void*
    {
        return registerTexture
            ? registerTexture(reinterpret_cast<void*>(sampler), reinterpret_cast<void*>(view),
                              static_cast<uint32_t>(layout))
            : nullptr;
    };
    m_viewContext.unregisterUiTexture = std::move(hooks.unregisterTexture);
}
#endif
