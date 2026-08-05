#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include <volk.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <functional>

inline void VkCheck(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

uint32_t VulkanFindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                              VkMemoryPropertyFlags required);

struct VulkanImageResource
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

VulkanImageResource VulkanCreateImage(VkPhysicalDevice physicalDevice, VkDevice device,
                                      uint32_t width, uint32_t height, VkFormat format,
                                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
void VulkanDestroyImage(VkDevice device, VulkanImageResource& image);

struct VulkanViewDeviceContext
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::function<void*(VkSampler, VkImageView, VkImageLayout)> registerUiTexture;
    std::function<void(void*)> unregisterUiTexture;
};
#endif
