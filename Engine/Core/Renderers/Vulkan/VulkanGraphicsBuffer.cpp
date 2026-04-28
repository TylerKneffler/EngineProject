#include "pch.h"
#include "VulkanGraphicsBuffer.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// VulkanGraphicsBuffer
// ---------------------------------------------------------------------------

VulkanGraphicsBuffer::VulkanGraphicsBuffer(
    VkDevice       device,
    VkBuffer       buffer,
    VkDeviceMemory memory,
    Usage          usage,
    AccessMode     access,
    uint64_t       size)
    : m_device(device)
    , m_buffer(buffer)
    , m_memory(memory)
    , m_usage(usage)
    , m_access(access)
    , m_size(size)
{
}

VulkanGraphicsBuffer::~VulkanGraphicsBuffer()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_mappedData)
        Unmap();

    if (m_device != VK_NULL_HANDLE)
    {
        if (m_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, m_buffer, nullptr);
        if (m_memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, m_memory, nullptr);
    }
#endif
}

void* VulkanGraphicsBuffer::Map()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_mappedData)
        return m_mappedData;

    if (m_access == AccessMode::Default)
    {
        OutputDebugStringA("[VulkanGraphicsBuffer] Cannot map a Default access buffer\n");
        return nullptr;
    }

    if (vkMapMemory(m_device, m_memory, 0, m_size, 0, &m_mappedData) != VK_SUCCESS)
    {
        OutputDebugStringA("[VulkanGraphicsBuffer] vkMapMemory failed\n");
        return nullptr;
    }

    return m_mappedData;
#else
    return nullptr;
#endif
}

void VulkanGraphicsBuffer::Unmap()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_mappedData && m_device != VK_NULL_HANDLE)
    {
        vkUnmapMemory(m_device, m_memory);
        m_mappedData = nullptr;
    }
#endif
}

// ---------------------------------------------------------------------------
// VulkanBufferFactory
// ---------------------------------------------------------------------------

#if defined(ENGINE_VULKAN_ENABLED)

uint32_t VulkanBufferFactory::FindMemoryType(uint32_t typeFilter,
                                              VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

#endif // ENGINE_VULKAN_ENABLED

std::unique_ptr<IGraphicsBuffer> VulkanBufferFactory::CreateBuffer(
    IGraphicsBuffer::Usage      usage,
    IGraphicsBuffer::AccessMode access,
    uint64_t                    sizeBytes,
    const void*                 initialData)
{
#if !defined(ENGINE_VULKAN_ENABLED)
    (void)usage; (void)access; (void)sizeBytes; (void)initialData;
    return nullptr;
#else
    if (m_device == VK_NULL_HANDLE || sizeBytes == 0)
        return nullptr;

    // Determine buffer usage flags
    VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    switch (usage)
    {
        case IGraphicsBuffer::Usage::ConstantBuffer:
            usageFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        case IGraphicsBuffer::Usage::VertexBuffer:
            usageFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case IGraphicsBuffer::Usage::IndexBuffer:
            usageFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case IGraphicsBuffer::Usage::ShaderResource:
            usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            break;
    }

    // Determine memory properties
    VkMemoryPropertyFlags memProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    switch (access)
    {
        case IGraphicsBuffer::AccessMode::Upload:
            memProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            usageFlags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            break;
        case IGraphicsBuffer::AccessMode::Readback:
            memProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        default:
            break;
    }

    // Create buffer
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = sizeBytes;
    bufInfo.usage       = usageFlags;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buffer) != VK_SUCCESS)
    {
        OutputDebugStringA("[VulkanBufferFactory] vkCreateBuffer failed\n");
        return nullptr;
    }

    // Allocate memory
    VkMemoryRequirements memReqs{};
    vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

    uint32_t memTypeIndex = FindMemoryType(memReqs.memoryTypeBits, memProps);
    if (memTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(m_device, buffer, nullptr);
        OutputDebugStringA("[VulkanBufferFactory] No suitable memory type found\n");
        return nullptr;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(m_device, buffer, nullptr);
        OutputDebugStringA("[VulkanBufferFactory] vkAllocateMemory failed\n");
        return nullptr;
    }

    vkBindBufferMemory(m_device, buffer, memory, 0);

    auto result = std::make_unique<VulkanGraphicsBuffer>(
        m_device, buffer, memory, usage, access, sizeBytes);

    // Upload initial data if provided (for CPU-visible buffers)
    if (initialData && access == IGraphicsBuffer::AccessMode::Upload)
    {
        void* mapped = result->Map();
        if (mapped)
        {
            memcpy(mapped, initialData, sizeBytes);
            result->Unmap();
        }
    }

    return result;
#endif
}
