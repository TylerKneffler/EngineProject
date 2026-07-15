#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGameRenderer.h"

bool VulkanGameRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    m_width = width; m_height = height;
    if (!m_core.Init(static_cast<HWND>(hwnd), width, height, true)) return false;
    m_provider = std::make_unique<VulkanGraphicsProvider>(
        m_core.GetPhysicalDevice(), m_core.GetDevice(), m_core.GetMainRenderPass());
    return true;
}

void VulkanGameRenderer::Resize(uint32_t width, uint32_t height)
{ if (width && height) { m_width = width; m_height = height; m_core.Resize(width, height); } }

void VulkanGameRenderer::BeginFrame()
{
    m_commandBuffer = m_core.BeginFrame();
    m_renderPassActive = false;
}

void VulkanGameRenderer::BeginMainRenderPass(const float color[4])
{
    if (!m_commandBuffer || m_renderPassActive) return;
    VkClearValue clears[2]{}; clears[0].color = { { color[0], color[1], color[2], color[3] } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = m_core.GetMainRenderPass(); begin.framebuffer = m_core.GetCurrentFramebuffer();
    begin.renderArea.extent = m_core.GetExtent(); begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(m_commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    auto extent = m_core.GetExtent();
    VkViewport viewport{ 0.0f, static_cast<float>(extent.height), static_cast<float>(extent.width), -static_cast<float>(extent.height), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, extent };
    vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport); vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
    m_renderPassActive = true;
}

void VulkanGameRenderer::Clear(float r, float g, float b, float a)
{ const float color[] = { r, g, b, a }; BeginMainRenderPass(color); }

void VulkanGameRenderer::EndFrame()
{
    if (!m_commandBuffer) return;
    if (!m_renderPassActive) { const float color[] = { 0.1f, 0.1f, 0.1f, 1.0f }; BeginMainRenderPass(color); }
    vkCmdEndRenderPass(m_commandBuffer);
    m_core.EndFrame(); m_commandBuffer = VK_NULL_HANDLE; m_renderPassActive = false;
}

std::unique_ptr<IGraphicsContext> VulkanGameRenderer::CreateFrameGraphicsContext()
{ return m_commandBuffer ? std::make_unique<VulkanGraphicsContext>(m_commandBuffer) : nullptr; }
#endif
