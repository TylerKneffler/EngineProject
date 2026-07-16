#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanView.h"

VulkanView::~VulkanView()
{
    if (m_context.device) vkDeviceWaitIdle(m_context.device);
    DestroyResources();
    if (m_sampler) vkDestroySampler(m_context.device, m_sampler, nullptr);
}

void VulkanView::Init(void* deviceContext, uint32_t width, uint32_t height,
                      void*, void*, uint32_t slot)
{
    m_context = *static_cast<VulkanViewDeviceContext*>(deviceContext);
    m_slot = slot;
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    VkCheck(vkCreateSampler(m_context.device, &samplerInfo, nullptr, &m_sampler), "vkCreateSampler");
    CreateResources(width, height);
}

void VulkanView::Resize(void*, uint32_t width, uint32_t height)
{
    if (!width || !height || (width == m_width && height == m_height)) return;
    vkDeviceWaitIdle(m_context.device);
    DestroyResources();
    CreateResources(width, height);
}

void VulkanView::CreateResources(uint32_t width, uint32_t height)
{
    m_width = width; m_height = height; m_aspect = static_cast<float>(width) / height;
    m_color = VulkanCreateImage(m_context.physicalDevice, m_context.device, width, height,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    m_depth = VulkanCreateImage(m_context.physicalDevice, m_context.device, width, height,
        VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView attachments[] = { m_color.view, m_depth.view };
    VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebufferInfo.renderPass = m_context.renderPass;
    framebufferInfo.attachmentCount = ARRAYSIZE(attachments);
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = width; framebufferInfo.height = height; framebufferInfo.layers = 1;
    VkCheck(vkCreateFramebuffer(m_context.device, &framebufferInfo, nullptr, &m_framebuffer), "vkCreateFramebuffer(view)");
    if (m_context.registerUiTexture)
        m_uiTexture = m_context.registerUiTexture(
            m_sampler, m_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanView::DestroyResources()
{
    if (m_uiTexture && m_context.unregisterUiTexture)
        m_context.unregisterUiTexture(m_uiTexture);
    if (m_framebuffer) vkDestroyFramebuffer(m_context.device, m_framebuffer, nullptr);
    VulkanDestroyImage(m_context.device, m_depth);
    VulkanDestroyImage(m_context.device, m_color);
    m_uiTexture = nullptr; m_framebuffer = VK_NULL_HANDLE;
}

void VulkanView::Render(void* commandBuffer, void*, std::function<void(void*)> drawFn)
{
    VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(commandBuffer);
    VkClearValue clears[2]{};
    clears[0].color = { { 0.0f, 0.0f, 0.502f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = m_context.renderPass; begin.framebuffer = m_framebuffer;
    begin.renderArea.extent = { m_width, m_height }; begin.clearValueCount = 2; begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{ 0.0f, static_cast<float>(m_height), static_cast<float>(m_width), -static_cast<float>(m_height), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, { m_width, m_height } };
    vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
    if (drawFn) drawFn(commandBuffer);
    vkCmdEndRenderPass(command);
}
#endif
