#include "pch.h"
#include "VulkanGraphicsProvider.h"
#include "VulkanShaderCompiler.h"
#include "VulkanGraphicsBuffer.h"
#include "VulkanPipelineStateBuilder.h"
#include "VulkanGraphicsContext.h"

// ---------------------------------------------------------------------------
// Constructor — creates all four sub-factories
// ---------------------------------------------------------------------------
VulkanGraphicsProvider::VulkanGraphicsProvider(VkDevice         device,
                                               VkPhysicalDevice physicalDevice,
                                               VkQueue          graphicsQueue,
                                               VkCommandPool    commandPool)
    : m_device(device)
    , m_physicalDevice(physicalDevice)
    , m_graphicsQueue(graphicsQueue)
    , m_commandPool(commandPool)
{
    OutputDebugStringA("[VulkanGraphicsProvider] Creating shader compiler...\n");
    m_shaderCompiler = std::make_unique<VulkanShaderCompiler>();

    OutputDebugStringA("[VulkanGraphicsProvider] Creating buffer factory...\n");
    m_bufferFactory = std::make_unique<VulkanBufferFactory>(m_device, m_physicalDevice);

    OutputDebugStringA("[VulkanGraphicsProvider] Creating pipeline factory...\n");
    m_pipelineFactory = std::make_unique<VulkanPipelineStateFactory>(m_device);

    OutputDebugStringA("[VulkanGraphicsProvider] Creating context factory...\n");
    m_contextFactory = std::make_unique<VulkanGraphicsContextFactory>(m_device, m_commandPool);

    OutputDebugStringA("[VulkanGraphicsProvider] All factories created\n");
}

// ---------------------------------------------------------------------------
// Destructor — unique_ptrs clean up sub-factories automatically
// ---------------------------------------------------------------------------
VulkanGraphicsProvider::~VulkanGraphicsProvider() = default;

// ---------------------------------------------------------------------------
// Factory accessors
// ---------------------------------------------------------------------------
IShaderCompiler*         VulkanGraphicsProvider::GetShaderCompiler()       { return m_shaderCompiler.get();  }
IGraphicsBufferFactory*  VulkanGraphicsProvider::GetBufferFactory()        { return m_bufferFactory.get();   }
IPipelineStateFactory*   VulkanGraphicsProvider::GetPipelineStateFactory() { return m_pipelineFactory.get(); }
IGraphicsContextFactory* VulkanGraphicsProvider::GetContextFactory()       { return m_contextFactory.get();  }


