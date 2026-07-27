#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IGraphicsProvider.h"
#include "VulkanShaderCompiler.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"
#include "VulkanGraphicsContext.h"
#include "VulkanGraphicsTexture.h"

class VulkanGraphicsProvider : public IGraphicsProvider
{
public:
    VulkanGraphicsProvider(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkRenderPass renderPass,
        VkQueue queue,
        uint32_t queueFamily);
    IShaderCompiler* GetShaderCompiler() override { return &m_shaderCompiler; }
    IGraphicsBufferFactory* GetBufferFactory() override { return m_bufferFactory.get(); }
    IPipelineStateFactory* GetPipelineStateFactory() override { return m_pipelineFactory.get(); }
    IGraphicsContextFactory* GetContextFactory() override { return &m_contextFactory; }
    IGraphicsTextureFactory* GetTextureFactory() override { return m_textureFactory.get(); }
private:
    VulkanShaderCompiler m_shaderCompiler;
    std::unique_ptr<VulkanBufferFactory> m_bufferFactory;
    std::unique_ptr<VulkanPipelineStateFactory> m_pipelineFactory;
    VulkanGraphicsContextFactory m_contextFactory;
    std::shared_ptr<VulkanTextureSystem> m_textureSystem;
    std::unique_ptr<VulkanTextureFactory> m_textureFactory;
};
#endif
