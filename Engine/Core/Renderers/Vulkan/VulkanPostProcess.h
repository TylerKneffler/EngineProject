#pragma once
#if defined(ENGINE_VULKAN_ENABLED)

#include "VulkanCommon.h"

namespace Engine::Renderers
{
class VulkanPostProcess
{
public:
    ~VulkanPostProcess();
    void Init(VkPhysicalDevice physicalDevice, VkDevice device,
        VkRenderPass sceneRenderPass, VkRenderPass outputRenderPass,
        uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);
    void BeginScene(VkCommandBuffer command, const float clearColor[4]);
    void EndScene(VkCommandBuffer command);
    void BeginComposition(VkCommandBuffer command, VkFramebuffer output,
        uint32_t width, uint32_t height, const float clearColor[4]);
    void EndComposition(VkCommandBuffer command);

private:
    void CreateImages(uint32_t width, uint32_t height);
    void DestroyImages();
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_sceneRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_outputRenderPass = VK_NULL_HANDLE;
    VulkanImageResource m_sceneColor;
    VulkanImageResource m_depth;
    VkFramebuffer m_sceneFramebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
};
}
#endif
