#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsBuffer.h"
#include "VulkanQueueMutex.h"


namespace Engine::Renderers
{
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

std::unique_ptr<Engine::Graphics::IGraphicsBuffer> VulkanBufferFactory::CreateBuffer(
    Engine::Graphics::IGraphicsBuffer::Usage usage, Engine::Graphics::IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes, const void* initialData, uint32_t elementStride)
{
    VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    switch (usage)
    {
        case Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer: vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer: vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::IndexBuffer: vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource: vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer: vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; break;
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
    auto result = std::make_unique<VulkanGraphicsBuffer>(
        m_device, buffer, memory, usage, access, sizeBytes, mapped);
    result->SetElementStride(elementStride);
    return result;
}

std::unique_ptr<Engine::Graphics::IGraphicsBuffer> VulkanBufferFactory::CreateStaticVertexBuffer(
    uint64_t sizeBytes, const void* initialData)
{
    using Buffer=Engine::Graphics::IGraphicsBuffer;
    if(!m_queue || !initialData || !sizeBytes)
        return IGraphicsBufferFactory::CreateStaticVertexBuffer(sizeBytes,initialData);
    auto staging=CreateBuffer(Buffer::Usage::VertexBuffer,Buffer::AccessMode::Upload,sizeBytes,initialData);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size=sizeBytes;
    info.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkCheck(vkCreateBuffer(m_device,&info,nullptr,&buffer),"vkCreateBuffer(static geometry)");
    VkDeviceMemory memory=VK_NULL_HANDLE;
    try {
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(m_device,buffer,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=VulkanFindMemoryType(m_physicalDevice,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(m_device,&allocation,nullptr,&memory),"vkAllocateMemory(static geometry)");
        VkCheck(vkBindBufferMemory(m_device,buffer,memory,0),"vkBindBufferMemory(static geometry)");
    } catch(...) {
        vkDestroyBuffer(m_device,buffer,nullptr);
        if(memory) vkFreeMemory(m_device,memory,nullptr);
        throw;
    }
    auto destination=std::make_unique<VulkanGraphicsBuffer>(m_device,buffer,memory,
        Buffer::Usage::VertexBuffer,Buffer::AccessMode::Default,sizeBytes,nullptr);
    struct UploadCommands {
        VkDevice device; VkCommandPool pool=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
        ~UploadCommands() {
            if(fence) vkDestroyFence(device,fence,nullptr);
            if(pool) vkDestroyCommandPool(device,pool,nullptr);
        }
    } upload{m_device};
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; poolInfo.queueFamilyIndex=m_queueFamily;
    VkCheck(vkCreateCommandPool(m_device,&poolInfo,nullptr,&upload.pool),"vkCreateCommandPool(static geometry)");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool=upload.pool; allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount=1;
    VkCommandBuffer commands=VK_NULL_HANDLE;
    VkCheck(vkAllocateCommandBuffers(m_device,&allocate,&commands),"vkAllocateCommandBuffers(static geometry)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(commands,&begin),"vkBeginCommandBuffer(static geometry)");
    VkBufferCopy copy{0,0,sizeBytes};
    vkCmdCopyBuffer(commands,static_cast<VulkanGraphicsBuffer*>(staging.get())->GetBuffer(),buffer,1,&copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=buffer; barrier.size=sizeBytes;
    vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,
        0,nullptr,1,&barrier,0,nullptr);
    VkCheck(vkEndCommandBuffer(commands),"vkEndCommandBuffer(static geometry)");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(m_device,&fenceInfo,nullptr,&upload.fence),"vkCreateFence(static geometry)");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
    {
        std::lock_guard queueLock(VulkanQueueMutex());
        VkCheck(vkQueueSubmit(m_queue,1,&submit,upload.fence),"vkQueueSubmit(static geometry)");
    }
    // Staging and command storage stay alive until the GPU copy completes.
    VkCheck(vkWaitForFences(m_device,1,&upload.fence,VK_TRUE,UINT64_MAX),"vkWaitForFences(static geometry)");
    return destination;
}

}
#endif
