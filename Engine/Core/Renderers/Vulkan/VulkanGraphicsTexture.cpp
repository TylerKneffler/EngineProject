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
    VkDescriptorSetLayoutBinding bindings[17]{};
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
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t binding = 11; binding < 13; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t binding = 15; binding < 17; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = ARRAYSIZE(bindings);
    layoutInfo.pBindings = bindings;
    VkCheck(vkCreateDescriptorSetLayout(
        m_device, &layoutInfo, nullptr, &m_layout), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize sizes[3]{
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 9 * 512 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 512 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * 512 }
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
    Shutdown();
}

void VulkanTextureSystem::RegisterTexture(VulkanGraphicsTexture* texture)
{
    if (texture) m_liveTextures.insert(texture);
}

void VulkanTextureSystem::UnregisterTexture(VulkanGraphicsTexture* texture)
{
    m_liveTextures.erase(texture);
}

void VulkanTextureSystem::Shutdown()
{
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    for (VulkanGraphicsTexture* texture : m_liveTextures)
        if (texture) texture->ReleaseImage(m_device);
    m_liveTextures.clear();
    VulkanDestroyImage(m_device, m_white);
    if (m_dummyBuffer) vkDestroyBuffer(m_device, m_dummyBuffer, nullptr);
    if (m_dummyBufferMemory) vkFreeMemory(m_device, m_dummyBufferMemory, nullptr);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_layout) vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    m_dummyBuffer = VK_NULL_HANDLE;
    m_dummyBufferMemory = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
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

std::shared_ptr<VulkanGraphicsTexture> VulkanTextureSystem::CreateDepthTexture(
    uint32_t width, uint32_t height)
{
    if (!width || !height) return nullptr;
    VulkanImageResource depth = VulkanCreateImage(m_physicalDevice, m_device,
        width, height, VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    VulkanImageResource color = VulkanCreateImage(m_physicalDevice, m_device,
        width, height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCheck(vkCreateRenderPass(m_device, &passInfo, nullptr, &renderPass),
        "vkCreateRenderPass(shadow)");
    VkImageView views[] = { color.view, depth.view };
    VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCheck(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer),
        "vkCreateFramebuffer(shadow)");
    auto texture = std::make_shared<VulkanGraphicsTexture>(shared_from_this(), depth);
    texture->SetDepthTargetResources(color, renderPass, framebuffer, width, height);
    return texture;
}

void VulkanTextureSystem::Bind(
    VkCommandBuffer commands,
    VkPipelineLayout pipelineLayout,
    const std::array<const VulkanGraphicsTexture*, 9>& textures,
    const std::array<const VulkanGraphicsBuffer*, 7>& buffers)
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

        VkDescriptorImageInfo images[9]{};
        VkWriteDescriptorSet writes[17]{};
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
        images[6].imageView = textures[6] ? textures[6]->GetView() : m_white.view;
        images[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[10] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[10].dstSet = set;
        writes[10].dstBinding = 10;
        writes[10].descriptorCount = 1;
        writes[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[10].pImageInfo = &images[6];
        images[7].imageView = textures[7] ? textures[7]->GetView() : m_white.view;
        images[7].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[13] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[13].dstSet = set;
        writes[13].dstBinding = 13;
        writes[13].descriptorCount = 1;
        writes[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[13].pImageInfo = &images[7];
        images[8].imageView = textures[8] ? textures[8]->GetView() : m_white.view;
        images[8].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[14] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[14].dstSet = set;
        writes[14].dstBinding = 14;
        writes[14].descriptorCount = 1;
        writes[14].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[14].pImageInfo = &images[8];
        VkDescriptorImageInfo samplerInfo{};
        samplerInfo.sampler = m_sampler;
        writes[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[6].dstSet = set;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[6].pImageInfo = &samplerInfo;
        VkDescriptorBufferInfo bufferInfos[7]{};
        for (uint32_t index = 0; index < 7; ++index)
        {
            const uint32_t binding = index < 3 ? index + 7 :
                (index < 5 ? index + 8 : index + 10);
            bufferInfos[index].buffer =
                buffers[index] ? buffers[index]->GetBuffer() : m_dummyBuffer;
            bufferInfos[index].range =
                buffers[index] ? buffers[index]->GetSize() : 16;
            writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[binding].dstSet = set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &bufferInfos[index];
        }
        vkUpdateDescriptorSets(m_device, ARRAYSIZE(writes), writes, 0, nullptr);
        m_sets.emplace(key, set);
    }
    vkCmdBindDescriptorSets(
        commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
        0, 1, &set, 0, nullptr);
}

VulkanGraphicsTexture::VulkanGraphicsTexture(
    std::shared_ptr<VulkanTextureSystem> system, VulkanImageResource image)
    : m_system(std::move(system)), m_image(image)
{
    if (m_system) m_system->RegisterTexture(this);
}

void VulkanGraphicsTexture::ReleaseImage(VkDevice device)
{
    if (device && m_framebuffer) vkDestroyFramebuffer(device, m_framebuffer, nullptr);
    if (device && m_renderPass) vkDestroyRenderPass(device, m_renderPass, nullptr);
    if (device) VulkanDestroyImage(device, m_depthColor);
    if (device) VulkanDestroyImage(device, m_image);
    m_framebuffer = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_depthColor = {};
    m_image = {};
}

void VulkanGraphicsTexture::SetDepthTargetResources(VulkanImageResource color,
    VkRenderPass renderPass, VkFramebuffer framebuffer,
    uint32_t width, uint32_t height)
{
    m_depthColor = color;
    m_renderPass = renderPass;
    m_framebuffer = framebuffer;
    m_width = width;
    m_height = height;
}

VulkanGraphicsTexture::~VulkanGraphicsTexture()
{
    if (m_system)
    {
        const VkDevice device = m_system->GetDevice();
        m_system->UnregisterTexture(this);
        ReleaseImage(device);
    }
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

std::shared_ptr<Engine::Graphics::IGraphicsTexture>
VulkanTextureFactory::CreateDepthTexture2D(uint32_t width, uint32_t height)
{
    return m_system ? m_system->CreateDepthTexture(width, height) : nullptr;
}
}
#endif
