#pragma once
#if defined(ENGINE_VULKAN_ENABLED)

#include "VulkanCommon.h"
#include "Core/Graphics/PostProcess.h"
#include <array>

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
    // Records the bloom passes (outside any render pass) before beginning
    // the output render pass and drawing the compose triangle.
    void BeginComposition(VkCommandBuffer command, VkFramebuffer output,
        uint32_t width, uint32_t height, const float clearColor[4]);
    void EndComposition(VkCommandBuffer command);
    VkImage GetSceneColor() const { return m_sceneColor.image; }

private:
    void CreateImages(uint32_t width, uint32_t height);
    void DestroyImages();
    void RenderBloom(VkCommandBuffer command);
    VkPipeline CreatePipeline(VkShaderModule vertex, VkShaderModule fragment,
        VkRenderPass renderPass, bool additive);
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_sceneRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_sceneLoadPass = VK_NULL_HANDLE;
    VkRenderPass m_outputRenderPass = VK_NULL_HANDLE;
    // Bloom levels: cleared for downsampling, loaded for additive upsampling.
    VkRenderPass m_bloomClearPass = VK_NULL_HANDLE;
    VkRenderPass m_bloomLoadPass = VK_NULL_HANDLE;
    VulkanImageResource m_sceneColor;
    VulkanImageResource m_depth;
    VulkanImageResource m_bloom;
    std::array<VkImageView, Engine::Graphics::MaxBloomLevels> m_bloomViews{};
    std::array<VkFramebuffer, Engine::Graphics::MaxBloomLevels> m_bloomFramebuffers{};
    uint32_t m_bloomLevels = 0;
    VkFramebuffer m_sceneFramebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // Binding 0 = scene, binding 2 = bloom level 0. Also the source set for
    // the first downsample.
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    // Binding 0 = bloom level k, for passes that read level k.
    std::array<VkDescriptorSet, Engine::Graphics::MaxBloomLevels> m_bloomSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomDownsamplePipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpsamplePipeline = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
};
}
#endif
