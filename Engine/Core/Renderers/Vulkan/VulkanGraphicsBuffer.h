#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IGraphicsBuffer.h"
#include "VulkanCommon.h"

class VulkanGraphicsBuffer : public IGraphicsBuffer
{
public:
    VulkanGraphicsBuffer(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                         Usage usage, AccessMode access, uint64_t size, void* mapped);
    ~VulkanGraphicsBuffer() override;
    Usage GetUsage() const override { return m_usage; }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t GetSize() const override { return m_size; }
    void* Map() override { return m_mapped; }
    void Unmap() override {}
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_buffer); }
    VkBuffer GetBuffer() const { return m_buffer; }
    const uint8_t* GetMappedData() const { return static_cast<const uint8_t*>(m_mapped); }
private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    Usage m_usage;
    AccessMode m_access;
    uint64_t m_size = 0;
    void* m_mapped = nullptr;
};

class VulkanBufferFactory : public IGraphicsBufferFactory
{
public:
    VulkanBufferFactory(VkPhysicalDevice physicalDevice, VkDevice device)
        : m_physicalDevice(physicalDevice), m_device(device) {}
    std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access, uint64_t sizeBytes,
        const void* initialData = nullptr) override;
private:
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
};
#endif
