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
    void CreateExposureImages();
    void DestroyExposureImages();
    // Meters the scene (outside any render pass); returns the compose set
    // that reads this frame's exposure target.
    VkDescriptorSet RenderExposure(VkCommandBuffer command,
        const Engine::Graphics::PostProcessSettings& settings);
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
    // Binding 0 = scene, binding 2 = bloom level 0, binding 3 = exposure 0.
    // Also the source set for the first downsample and exposure metering.
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    // As m_descriptorSet with binding 3 = exposure 1.
    VkDescriptorSet m_composeSet1 = VK_NULL_HANDLE;
    // Binding 0 = bloom level k, for passes that read level k.
    std::array<VkDescriptorSet, Engine::Graphics::MaxBloomLevels> m_bloomSets{};
    // Auto exposure (Graphics/PostProcess.h): log-luminance map and
    // histogram (HDR format) and two 1x1 RGBA32F exposure targets that
    // alternate frames. Size-independent; created once.
    VulkanImageResource m_exposureMap;
    VulkanImageResource m_exposureHistogram;
    std::array<VulkanImageResource, 2> m_exposure{};
    VkFramebuffer m_exposureMapFramebuffer = VK_NULL_HANDLE;
    VkFramebuffer m_exposureHistogramFramebuffer = VK_NULL_HANDLE;
    std::array<VkFramebuffer, 2> m_exposureFramebuffers{};
    VkRenderPass m_exposurePass = VK_NULL_HANDLE; // RGBA32F, cleared
    // Binding 0 = exposure map (histogram pass); adapt set i: binding 0 =
    // histogram, binding 2 = exposure 1 - i (history), writes exposure i.
    VkDescriptorSet m_exposureMapSet = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> m_adaptSets{};
    VkPipeline m_exposureLuminancePipeline = VK_NULL_HANDLE;
    VkPipeline m_exposureHistogramPipeline = VK_NULL_HANDLE;
    VkPipeline m_exposureAdaptPipeline = VK_NULL_HANDLE;
    Engine::Graphics::ExposureState m_exposureState;
    // Exposure images start undefined; the first composition clears them
    // into the shader-read layout every descriptor set expects.
    bool m_exposureImagesReady = false;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomDownsamplePipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpsamplePipeline = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
};
}
#endif
