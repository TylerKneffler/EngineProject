#pragma once
#include "../../Graphics/IGraphicsProvider.h"

#ifdef VK_USE_PLATFORM_WIN32_KHR
    #include <vulkan/vulkan.h>
#else
    // Minimal Vulkan type stubs for compilation when Vulkan SDK is not installed
    typedef void* VkDevice;
    typedef void* VkQueue;
    typedef void* VkCommandPool;
    
    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>

class VulkanShaderCompiler;
class VulkanGraphicsBuffer;
class VulkanPipelineStateBuilder;
class VulkanGraphicsContext;

// ---------------------------------------------------------------------------
// VulkanGraphicsProvider — Vulkan implementation of IGraphicsProvider
//
// Factory for creating Vulkan graphics resources (shaders, buffers, pipelines,
// command contexts). Manages Vulkan device, queues, and memory allocators.
// ---------------------------------------------------------------------------
class VulkanGraphicsProvider : public IGraphicsProvider
{
public:
    // Constructor takes Vulkan device and command queue
    VulkanGraphicsProvider(VkDevice device, VkQueue graphicsQueue, VkCommandPool commandPool);
    virtual ~VulkanGraphicsProvider();

    // IGraphicsProvider interface
    IShaderCompiler* GetShaderCompiler() override { return nullptr; /* TODO */ }
    IGraphicsBufferFactory* GetBufferFactory() override { return nullptr; /* TODO */ }
    IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; /* TODO */ }
    IGraphicsContextFactory* GetContextFactory() override { return nullptr; /* TODO */ }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;

    // TODO: Add factory member variables
    // std::unique_ptr<VulkanShaderCompiler> m_shaderCompiler;
    // std::unique_ptr<VulkanBufferFactory> m_bufferFactory;
    // std::unique_ptr<VulkanPipelineStateFactory> m_pipelineFactory;
    // std::unique_ptr<VulkanGraphicsContextFactory> m_contextFactory;
};
