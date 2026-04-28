#pragma once
#include "../../Graphics/IGraphicsProvider.h"

#if defined(ENGINE_VULKAN_ENABLED)
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR 1
    #endif
    #include <vulkan/vulkan.h>
#else
    // Minimal Vulkan type stubs for compilation when Vulkan SDK is not installed
    typedef void* VkDevice;
    typedef void* VkPhysicalDevice;
    typedef void* VkQueue;
    typedef void* VkCommandPool;

    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>

class VulkanShaderCompiler;
class VulkanBufferFactory;
class VulkanPipelineStateFactory;
class VulkanGraphicsContextFactory;

// ---------------------------------------------------------------------------
// VulkanGraphicsProvider — Vulkan implementation of IGraphicsProvider
//
// Factory for creating Vulkan graphics resources (shaders, buffers, pipelines,
// command contexts).  All four sub-factories are created in the constructor.
// ---------------------------------------------------------------------------
class VulkanGraphicsProvider : public IGraphicsProvider
{
public:
    VulkanGraphicsProvider(VkDevice         device,
                           VkPhysicalDevice physicalDevice,
                           VkQueue          graphicsQueue,
                           VkCommandPool    commandPool);
    virtual ~VulkanGraphicsProvider();

    // IGraphicsProvider interface
    IShaderCompiler*         GetShaderCompiler()       override;
    IGraphicsBufferFactory*  GetBufferFactory()        override;
    IPipelineStateFactory*   GetPipelineStateFactory() override;
    IGraphicsContextFactory* GetContextFactory()       override;

private:
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue  = VK_NULL_HANDLE;
    VkCommandPool    m_commandPool    = VK_NULL_HANDLE;

    std::unique_ptr<VulkanShaderCompiler>        m_shaderCompiler;
    std::unique_ptr<VulkanBufferFactory>         m_bufferFactory;
    std::unique_ptr<VulkanPipelineStateFactory>  m_pipelineFactory;
    std::unique_ptr<VulkanGraphicsContextFactory> m_contextFactory;
};
