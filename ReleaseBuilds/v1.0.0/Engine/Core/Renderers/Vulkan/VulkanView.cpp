#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanView.h"

namespace Engine::Renderers
{
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
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageView attachments[] = { m_color.view, m_depth.view };
    VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebufferInfo.renderPass = m_context.compositionRenderPass;
    framebufferInfo.attachmentCount = ARRAYSIZE(attachments);
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = width; framebufferInfo.height = height; framebufferInfo.layers = 1;
    VkCheck(vkCreateFramebuffer(m_context.device, &framebufferInfo, nullptr, &m_framebuffer), "vkCreateFramebuffer(view)");
    if (!m_postInitialized)
    {
        m_postProcess.Init(m_context.physicalDevice, m_context.device,
            m_context.renderPass, m_context.compositionRenderPass, width, height);
        m_postInitialized = true;
    }
    else
        m_postProcess.Resize(width, height);
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

void VulkanView::Render(void* commandBuffer, void*,
    std::function<void(void*)> drawFn,
    std::function<void(void*)> preDrawFn)
{
    VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(commandBuffer);
    if (preDrawFn) preDrawFn(commandBuffer);
    m_postProcess.BeginScene(command, m_clearColor);
    if (drawFn) drawFn(commandBuffer);
    m_postProcess.EndScene(command);
    m_postProcess.BeginComposition(command, m_framebuffer,
        m_width, m_height, m_clearColor);
    m_postProcess.EndComposition(command);
}
}
#endif
