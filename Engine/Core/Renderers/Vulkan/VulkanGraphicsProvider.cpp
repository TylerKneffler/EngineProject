#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsProvider.h"
VulkanGraphicsProvider::VulkanGraphicsProvider(VkPhysicalDevice physicalDevice, VkDevice device, VkRenderPass renderPass)
{
    m_bufferFactory = std::make_unique<VulkanBufferFactory>(physicalDevice, device);
    m_pipelineFactory = std::make_unique<VulkanPipelineStateFactory>(device, renderPass);
}
#endif
