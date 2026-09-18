#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsProvider.h"
#include <vector>

namespace Engine::Renderers
{
VulkanGraphicsProvider::~VulkanGraphicsProvider()
{
    if (m_textureSystem) m_textureSystem->Shutdown();
}

VulkanGraphicsProvider::VulkanGraphicsProvider(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkRenderPass renderPass,
    VkQueue queue,
    uint32_t queueFamily)
{
    m_textureSystem = std::make_shared<VulkanTextureSystem>(
        physicalDevice, device, queue, queueFamily);
    m_bufferFactory = std::make_unique<VulkanBufferFactory>(physicalDevice, device);
    m_pipelineFactory = std::make_unique<VulkanPipelineStateFactory>(
        device, renderPass, m_textureSystem->GetDescriptorSetLayout());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
        nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
        queueFamilies.data());
    const bool timestampsSupported = queueFamily < queueFamilyCount &&
        queueFamilies[queueFamily].timestampValidBits != 0;
    m_contextFactory.SetDevice(device,
        timestampsSupported ? properties.limits.timestampPeriod : 0.0f);
    m_contextFactory.SetTextureSystem(m_textureSystem);
    m_textureFactory = std::make_unique<VulkanTextureFactory>(m_textureSystem);
}
}
#endif
