#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"

#if defined(ENGINE_VULKAN_ENABLED)
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR 1
    #endif
    #include <vulkan/vulkan.h>
#else
    typedef void* VkDevice;
    typedef void* VkPhysicalDevice;
    typedef void* VkBuffer;
    typedef void* VkDeviceMemory;
    typedef unsigned int VkMemoryPropertyFlags;
    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>

// ---------------------------------------------------------------------------
// VulkanGraphicsBuffer — VkBuffer + VkDeviceMemory wrapper
// ---------------------------------------------------------------------------
class VulkanGraphicsBuffer : public IGraphicsBuffer
{
public:
    VulkanGraphicsBuffer(
        VkDevice       device,
        VkBuffer       buffer,
        VkDeviceMemory memory,
        Usage          usage,
        AccessMode     access,
        uint64_t       size);

    ~VulkanGraphicsBuffer() override;

    Usage      GetUsage()      const override { return m_usage;  }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t   GetSize()       const override { return m_size;   }

    void* Map()   override;
    void  Unmap() override;

    void* GetNativeHandle() const override { return static_cast<void*>(m_buffer); }
    VkBuffer GetVkBuffer()  const          { return m_buffer; }

private:
    VkDevice       m_device     = VK_NULL_HANDLE;
    VkBuffer       m_buffer     = VK_NULL_HANDLE;
    VkDeviceMemory m_memory     = VK_NULL_HANDLE;
    Usage          m_usage;
    AccessMode     m_access;
    uint64_t       m_size       = 0;
    void*          m_mappedData = nullptr;
};

// ---------------------------------------------------------------------------
// VulkanBufferFactory — Creates Vulkan GPU buffers
// ---------------------------------------------------------------------------
class VulkanBufferFactory : public IGraphicsBufferFactory
{
public:
    VulkanBufferFactory(VkDevice device, VkPhysicalDevice physicalDevice)
        : m_device(device), m_physicalDevice(physicalDevice) {}

    std::unique_ptr<IGraphicsBuffer> CreateBuffer(
        IGraphicsBuffer::Usage      usage,
        IGraphicsBuffer::AccessMode access,
        uint64_t                    sizeBytes,
        const void*                 initialData = nullptr) override;

private:
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;

#if defined(ENGINE_VULKAN_ENABLED)
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
#endif
};
