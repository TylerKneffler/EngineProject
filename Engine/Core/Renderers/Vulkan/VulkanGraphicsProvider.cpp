#include "pch.h"
#include "VulkanGraphicsProvider.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
VulkanGraphicsProvider::VulkanGraphicsProvider(VkDevice device,
                                               VkQueue graphicsQueue,
                                               VkCommandPool commandPool)
    : m_device(device)
    , m_graphicsQueue(graphicsQueue)
    , m_commandPool(commandPool)
{
    // TODO: Initialize factory members
    // - Create VulkanShaderCompiler
    // - Create VulkanBufferFactory
    // - Create VulkanPipelineStateFactory
    // - Create VulkanGraphicsContextFactory
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
VulkanGraphicsProvider::~VulkanGraphicsProvider()
{
    // Cleanup handled by unique_ptr members
}

