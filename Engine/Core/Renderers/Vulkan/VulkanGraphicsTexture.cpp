#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGraphicsTexture.h"
#include "VulkanGraphicsBuffer.h"

#include <cstring>

namespace Engine::Renderers
{
size_t VulkanTextureSystem::TextureKeyHash::operator()(const TextureKey& key) const
{
    size_t hash = 0;
    for (VkImageView value : key.views)
        hash ^= std::hash<VkImageView>{}(value) +
            0x9e3779b9 + (hash << 6) + (hash >> 2);
    for (VkBuffer value : key.buffers)
        hash ^= std::hash<VkBuffer>{}(value) +
            0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
}

VulkanTextureSystem::VulkanTextureSystem(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkQueue queue,
    uint32_t queueFamily)
    : m_physicalDevice(physicalDevice),
      m_device(device),
      m_queue(queue),
      m_queueFamily(queueFamily)
{
    VkDescriptorSetLayoutBinding bindings[10]{};
    for (uint32_t binding = 0; binding < 6; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t binding = 7; binding < 10; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = ARRAYSIZE(bindings);
    layoutInfo.pBindings = bindings;
    VkCheck(vkCreateDescriptorSetLayout(
        m_device, &layoutInfo, nullptr, &m_layout), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize sizes[3]{
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 * 512 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 512 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 512 }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 512;
    poolInfo.poolSizeCount = ARRAYSIZE(sizes);
    poolInfo.pPoolSizes = sizes;
    VkCheck(vkCreateDescriptorPool(
        m_device, &poolInfo, nullptr, &m_pool), "vkCreateDescriptorPool");

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkCheck(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler),
        "vkCreateSampler");

    VkBufferCreateInfo dummyInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    dummyInfo.size = 16;
    dummyInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    dummyInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkCheck(vkCreateBuffer(m_device, &dummyInfo, nullptr, &m_dummyBuffer),
        "vkCreateBuffer(dummy storage)");
    VkMemoryRequirements dummyRequirements{};
    vkGetBufferMemoryRequirements(m_device, m_dummyBuffer, &dummyRequirements);
    VkMemoryAllocateInfo dummyAllocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    dummyAllocation.allocationSize = dummyRequirements.size;
    dummyAllocation.memoryTypeIndex = VulkanFindMemoryType(
        m_physicalDevice, dummyRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCheck(vkAllocateMemory(
        m_device, &dummyAllocation, nullptr, &m_dummyBufferMemory),
        "vkAllocateMemory(dummy storage)");
    VkCheck(vkBindBufferMemory(
        m_device, m_dummyBuffer, m_dummyBufferMemory, 0),
        "vkBindBufferMemory(dummy storage)");

    const uint8_t white[] = { 255, 255, 255, 255 };
    m_white = Upload(1, 1, white, 1, Engine::Graphics::GraphicsTextureFormat::Rgba8);
}

VulkanTextureSystem::~VulkanTextureSystem()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device);
        VulkanDestroyImage(m_device, m_white);
        if (m_dummyBuffer) vkDestroyBuffer(m_device, m_dummyBuffer, nullptr);
        if (m_dummyBufferMemory) vkFreeMemory(m_device, m_dummyBufferMemory, nullptr);
        if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
        if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        if (m_layout) vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }
}

VulkanImageResource VulkanTextureSystem::Upload(
    uint32_t width,
    uint32_t height,
    const uint8_t* pixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat textureFormat,
    bool srgb)
{
    const uint32_t bytesPerPixel = GraphicsTextureBytesPerPixel(textureFormat);
    VkDeviceSize size = 0;
    uint32_t mipWidth = width, mipHeight = height;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        size += static_cast<VkDeviceSize>(mipWidth) * mipHeight * bytesPerPixel;
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkCheck(vkCreateBuffer(m_device, &bufferInfo, nullptr, &staging),
        "vkCreateBuffer(texture staging)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, staging, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = VulkanFindMemoryType(
        m_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCheck(vkAllocateMemory(m_device, &allocation, nullptr, &stagingMemory),
        "vkAllocateMemory(texture staging)");
    VkCheck(vkBindBufferMemory(m_device, staging, stagingMemory, 0),
        "vkBindBufferMemory(texture staging)");
    void* mapped = nullptr;
    VkCheck(vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped),
        "vkMapMemory(texture staging)");
    std::memcpy(mapped, pixels, static_cast<size_t>(size));
    vkUnmapMemory(m_device, stagingMemory);

    VulkanImageResource image = VulkanCreateImage(
        m_physicalDevice, m_device, width, height,
        textureFormat == Engine::Graphics::GraphicsTextureFormat::Rgba32Float
            ? VK_FORMAT_R32G32B32A32_SFLOAT
            : (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM),
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    VkCheck(vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool),
        "vkCreateCommandPool(texture)");
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    commandInfo.commandPool = pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCheck(vkAllocateCommandBuffers(m_device, &commandInfo, &commands),
        "vkAllocateCommandBuffers(texture)");
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer(texture)");

    VkImageMemoryBarrier toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toCopy.srcAccessMask = 0;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = image.image;
    toCopy.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 };
    vkCmdPipelineBarrier(
        commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toCopy);

    std::vector<VkBufferImageCopy> copies(mipLevels);
    VkDeviceSize copyOffset = 0;
    mipWidth = width;
    mipHeight = height;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        copies[mip].bufferOffset = copyOffset;
        copies[mip].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
        copies[mip].imageExtent = { mipWidth, mipHeight, 1 };
        copyOffset += static_cast<VkDeviceSize>(mipWidth) * mipHeight * bytesPerPixel;
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }
    vkCmdCopyBufferToImage(
        commands, staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        mipLevels, copies.data());

    VkImageMemoryBarrier toShader = toCopy;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(
        commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);
    VkCheck(vkEndCommandBuffer(commands), "vkEndCommandBuffer(texture)");
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    VkCheck(vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(texture)");
    VkCheck(vkQueueWaitIdle(m_queue), "vkQueueWaitIdle(texture)");

    vkDestroyCommandPool(m_device, pool, nullptr);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    return image;
}

std::shared_ptr<VulkanGraphicsTexture> VulkanTextureSystem::CreateTexture(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format,
    bool srgb)
{
    if (!width || !height || !rgbaPixels || !mipLevels)
        return nullptr;
    return std::make_shared<VulkanGraphicsTexture>(
        shared_from_this(), Upload(width, height, rgbaPixels, mipLevels, format, srgb));
}

void VulkanTextureSystem::Bind(
    VkCommandBuffer commands,
    VkPipelineLayout pipelineLayout,
    const std::array<const VulkanGraphicsTexture*, 6>& textures,
    const std::array<const VulkanGraphicsBuffer*, 3>& buffers)
{
    TextureKey key{};
    for (size_t index = 0; index < textures.size(); ++index)
        key.views[index] =
            textures[index] ? textures[index]->GetView() : m_white.view;
    for (size_t index = 0; index < buffers.size(); ++index)
        key.buffers[index] =
            buffers[index] ? buffers[index]->GetBuffer() : m_dummyBuffer;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (auto found = m_sets.find(key); found != m_sets.end())
        set = found->second;
    else
    {
        VkDescriptorSetAllocateInfo allocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocate.descriptorPool = m_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_layout;
        VkCheck(vkAllocateDescriptorSets(m_device, &allocate, &set),
            "vkAllocateDescriptorSets(material)");

        VkDescriptorImageInfo images[6]{};
        VkWriteDescriptorSet writes[10]{};
        for (uint32_t index = 0; index < 6; ++index)
        {
            images[index].imageView =
                textures[index] ? textures[index]->GetView() : m_white.view;
            images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[index].dstSet = set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[index].pImageInfo = &images[index];
        }
        VkDescriptorImageInfo samplerInfo{};
        samplerInfo.sampler = m_sampler;
        writes[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[6].dstSet = set;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[6].pImageInfo = &samplerInfo;
        VkDescriptorBufferInfo bufferInfos[3]{};
        for (uint32_t index = 0; index < 3; ++index)
        {
            bufferInfos[index].buffer =
                buffers[index] ? buffers[index]->GetBuffer() : m_dummyBuffer;
            bufferInfos[index].range =
                buffers[index] ? buffers[index]->GetSize() : 16;
            writes[index + 7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[index + 7].dstSet = set;
            writes[index + 7].dstBinding = index + 7;
            writes[index + 7].descriptorCount = 1;
            writes[index + 7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index + 7].pBufferInfo = &bufferInfos[index];
        }
        vkUpdateDescriptorSets(m_device, ARRAYSIZE(writes), writes, 0, nullptr);
        m_sets.emplace(key, set);
    }
    vkCmdBindDescriptorSets(
        commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
        0, 1, &set, 0, nullptr);
}

VulkanGraphicsTexture::~VulkanGraphicsTexture()
{
    if (m_system)
        VulkanDestroyImage(m_system->GetDevice(), m_image);
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> VulkanTextureFactory::CreateTexture2D(
    uint32_t width,
    uint32_t height,
    const uint8_t* rgbaPixels,
    uint32_t mipLevels,
    Engine::Graphics::GraphicsTextureFormat format,
    bool srgb)
{
    return m_system
        ? m_system->CreateTexture(width, height, rgbaPixels, mipLevels, format, srgb)
        : nullptr;
}
}
#endif
