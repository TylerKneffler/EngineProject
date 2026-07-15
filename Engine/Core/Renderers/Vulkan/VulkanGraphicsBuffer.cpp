#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsBuffer.h"
#include <cstring>

VulkanGraphicsBuffer::VulkanGraphicsBuffer(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    Usage usage, AccessMode access, uint64_t size, void* mapped)
    : m_device(device), m_buffer(buffer), m_memory(memory), m_usage(usage),
      m_access(access), m_size(size), m_mapped(mapped) {}

VulkanGraphicsBuffer::~VulkanGraphicsBuffer()
{
    if (m_mapped) vkUnmapMemory(m_device, m_memory);
    if (m_buffer) vkDestroyBuffer(m_device, m_buffer, nullptr);
    if (m_memory) vkFreeMemory(m_device, m_memory, nullptr);
}

std::unique_ptr<IGraphicsBuffer> VulkanBufferFactory::CreateBuffer(
    IGraphicsBuffer::Usage usage, IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes, const void* initialData)
{
    VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    switch (usage)
    {
        case IGraphicsBuffer::Usage::ConstantBuffer: vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case IGraphicsBuffer::Usage::VertexBuffer: vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case IGraphicsBuffer::Usage::IndexBuffer: vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case IGraphicsBuffer::Usage::ShaderResource: vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
    }
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = sizeBytes;
    bufferInfo.usage = vkUsage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkCheck(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = VulkanFindMemoryType(m_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCheck(vkAllocateMemory(m_device, &allocation, nullptr, &memory), "vkAllocateMemory(buffer)");
    VkCheck(vkBindBufferMemory(m_device, buffer, memory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    VkCheck(vkMapMemory(m_device, memory, 0, sizeBytes, 0, &mapped), "vkMapMemory");
    if (initialData) std::memcpy(mapped, initialData, static_cast<size_t>(sizeBytes));
    return std::make_unique<VulkanGraphicsBuffer>(m_device, buffer, memory, usage, access, sizeBytes, mapped);
}
#endif
