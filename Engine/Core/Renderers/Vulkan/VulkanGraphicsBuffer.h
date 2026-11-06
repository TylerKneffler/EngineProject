#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Graphics/IGraphicsBuffer.h"
#include "VulkanCommon.h"
#include <atomic>

namespace Engine::Renderers
{
class VulkanGraphicsBuffer : public Engine::Graphics::IGraphicsBuffer
{
public:
    VulkanGraphicsBuffer(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                         Usage usage, AccessMode access, uint64_t size, void* mapped);
    ~VulkanGraphicsBuffer() override;
    Usage GetUsage() const override { return m_usage; }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t GetSize() const override { return m_size; }
    uint32_t GetElementStride() const override { return m_elementStride; }
    void SetElementStride(uint32_t value) { m_elementStride = value; }
    void* Map() override { return m_access == AccessMode::Default ? nullptr : m_mapped; }
    void Unmap() override {}
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_buffer); }
    VkBuffer GetBuffer() const { return m_buffer; }
    uint64_t GetIdentity() const { return m_identity; }
    const uint8_t* GetMappedData() const { return static_cast<const uint8_t*>(m_mapped); }
private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    Usage m_usage;
    AccessMode m_access;
    uint64_t m_size = 0;
    void* m_mapped = nullptr;
    uint32_t m_elementStride = 0;
    inline static std::atomic<uint64_t> s_nextIdentity{1};
    uint64_t m_identity = s_nextIdentity.fetch_add(1,std::memory_order_relaxed);
};

class VulkanBufferFactory : public Engine::Graphics::IGraphicsBufferFactory
{
public:
    VulkanBufferFactory(VkPhysicalDevice physicalDevice, VkDevice device,
        VkQueue queue = VK_NULL_HANDLE, uint32_t queueFamily = 0)
        : m_queue(queue), m_queueFamily(queueFamily), m_physicalDevice(physicalDevice), m_device(device) {}
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateBuffer(Engine::Graphics::IGraphicsBuffer::Usage usage,
        Engine::Graphics::IGraphicsBuffer::AccessMode access, uint64_t sizeBytes,
        const void* initialData = nullptr,
        uint32_t elementStride = 0) override;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateStaticVertexBuffer(
        uint64_t sizeBytes, const void* initialData) override;
private:
    VkQueue m_queue;
    uint32_t m_queueFamily;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
};
}
#endif
