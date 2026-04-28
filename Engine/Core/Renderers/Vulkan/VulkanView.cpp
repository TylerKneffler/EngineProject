#include "pch.h"
#include "VulkanView.h"

#if defined(ENGINE_VULKAN_ENABLED)
namespace
{
// ---------------------------------------------------------------------------
// Helper: find a suitable device memory type
// ---------------------------------------------------------------------------
static uint32_t FindMemoryType(VkPhysicalDevice physDev,
                                uint32_t         typeFilter,
                                VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Helper: create a VkImage + allocate and bind VkDeviceMemory
// ---------------------------------------------------------------------------
static bool CreateImage(VkDevice               device,
                        VkPhysicalDevice       physDev,
                        uint32_t               width,
                        uint32_t               height,
                        VkFormat               format,
                        VkImageUsageFlags      usage,
                        VkMemoryPropertyFlags  memProps,
                        VkImage&               outImage,
                        VkDeviceMemory&        outMemory)
{
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.extent        = { width, height, 1 };
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.format        = format;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage         = usage;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device, &imgInfo, nullptr, &outImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs{};
    vkGetImageMemoryRequirements(device, outImage, &memReqs);

    uint32_t typeIndex = FindMemoryType(physDev, memReqs.memoryTypeBits, memProps);
    if (typeIndex == UINT32_MAX)
    {
        vkDestroyImage(device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
    {
        vkDestroyImage(device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(device, outImage, outMemory, 0);
    return true;
}
} // anonymous namespace
#endif // ENGINE_VULKAN_ENABLED

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
VulkanView::~VulkanView()
{
    CleanupResources();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void VulkanView::Init(void* device,
                      uint32_t width, uint32_t height,
                      void* srvCpu, void* srvGpu,
                      uint32_t srvSlotIndex)
{
    m_device       = static_cast<VkDevice>(device);
    m_srvSlotIndex = srvSlotIndex;
    (void)srvCpu; (void)srvGpu; // Vulkan uses VkDescriptorSet, not CPU/GPU handles
    CreateResources(device, width, height);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void VulkanView::Resize(void* device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)              return;
    if (width == m_width && height == m_height) return;

    CleanupResources();
    m_device = static_cast<VkDevice>(device);
    CreateResources(device, width, height);
}

// ---------------------------------------------------------------------------
// CreateResources
// ---------------------------------------------------------------------------
void VulkanView::CreateResources(void* /*device*/, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;
    m_aspect = (height > 0)
        ? static_cast<float>(width) / static_cast<float>(height)
        : 1.0f;

#if !defined(ENGINE_VULKAN_ENABLED)
    return;
#else
    if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE)
    {
        OutputDebugStringA("[VulkanView] CreateResources: device or physicalDevice is null\n");
        return;
    }

    // --- Color image (RGBA8, SAMPLED so ImGui can display it) ---
    constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    if (!CreateImage(m_device, m_physicalDevice, width, height,
                     COLOR_FORMAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_colorImage, m_colorMemory))
    {
        OutputDebugStringA("[VulkanView] Failed to create color image\n");
        return;
    }

    // --- Depth image ---
    if (!CreateImage(m_device, m_physicalDevice, width, height,
                     DEPTH_FORMAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_depthImage, m_depthMemory))
    {
        OutputDebugStringA("[VulkanView] Failed to create depth image\n");
        return;
    }

    // --- Color image view ---
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_colorImage;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = COLOR_FORMAT;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_colorImageView) != VK_SUCCESS)
        {
            OutputDebugStringA("[VulkanView] Failed to create color image view\n");
            return;
        }
    }

    // --- Depth image view ---
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = m_depthImage;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = DEPTH_FORMAT;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
        {
            OutputDebugStringA("[VulkanView] Failed to create depth image view\n");
            return;
        }
    }

    // --- Render pass ---
    // Color: clear on load, store for shader-read; Depth: clear on load, discard.
    // The render pass handles layout transitions (UNDEFINED → attachment → SRV).
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = COLOR_FORMAT;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription depthAtt{};
        depthAtt.format         = DEPTH_FORMAT;
        depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkAttachmentDescription attachments[] = { colorAtt, depthAtt };

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies   = &dep;

        if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            OutputDebugStringA("[VulkanView] Failed to create render pass\n");
            return;
        }
    }

    // --- Framebuffer ---
    {
        VkImageView attachments[] = { m_colorImageView, m_depthImageView };

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments    = attachments;
        fbInfo.width           = width;
        fbInfo.height          = height;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) != VK_SUCCESS)
        {
            OutputDebugStringA("[VulkanView] Failed to create framebuffer\n");
            return;
        }
    }

    // m_descriptorSet remains null — ImGui Vulkan texture display requires
    // ImGui_ImplVulkan_AddTexture() which needs the Vulkan ImGui backend.
    // Add imgui_impl_vulkan.cpp to the ImGui CMake target to enable it.
#endif // ENGINE_VULKAN_ENABLED
}

// ---------------------------------------------------------------------------
// CleanupResources
// ---------------------------------------------------------------------------
void VulkanView::CleanupResources()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_device == VK_NULL_HANDLE) return;

    if (m_framebuffer    != VK_NULL_HANDLE) { vkDestroyFramebuffer(m_device, m_framebuffer,    nullptr); m_framebuffer    = VK_NULL_HANDLE; }
    if (m_renderPass     != VK_NULL_HANDLE) { vkDestroyRenderPass(m_device, m_renderPass,      nullptr); m_renderPass     = VK_NULL_HANDLE; }
    if (m_colorImageView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_colorImageView,   nullptr); m_colorImageView = VK_NULL_HANDLE; }
    if (m_depthImageView != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_depthImageView,   nullptr); m_depthImageView = VK_NULL_HANDLE; }
    if (m_colorImage     != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_colorImage,           nullptr); m_colorImage     = VK_NULL_HANDLE; }
    if (m_depthImage     != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_depthImage,           nullptr); m_depthImage     = VK_NULL_HANDLE; }
    if (m_colorMemory    != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_colorMemory,            nullptr); m_colorMemory    = VK_NULL_HANDLE; }
    if (m_depthMemory    != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_depthMemory,            nullptr); m_depthMemory    = VK_NULL_HANDLE; }
#endif
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void VulkanView::Render(void* cmdList, void* /*mainRtv*/,
                        std::function<void(void*)> drawFn)
{
#if !defined(ENGINE_VULKAN_ENABLED)
    if (drawFn) drawFn(cmdList);
    return;
#else
    if (m_device == VK_NULL_HANDLE || m_framebuffer == VK_NULL_HANDLE) return;

    auto* cmdBuffer = static_cast<VkCommandBuffer>(cmdList);

    // Clear values: background colour + full depth
    VkClearValue clearValues[2];
    clearValues[0].color        = {{ 0.1f, 0.1f, 0.1f, 1.0f }};
    clearValues[1].depthStencil = { 1.0f, 0 };

    // Begin render pass — the pass handles all image-layout transitions:
    //   color: UNDEFINED → COLOR_ATTACHMENT → SHADER_READ_ONLY
    //   depth: UNDEFINED → DEPTH_STENCIL_ATTACHMENT
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffer;
    rpBegin.renderArea.offset = { 0, 0 };
    rpBegin.renderArea.extent = { m_width, m_height };
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = clearValues;

    vkCmdBeginRenderPass(cmdBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Set dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(m_width);
    viewport.height   = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

    VkRect2D scissor{ { 0, 0 }, { m_width, m_height } };
    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

    // Invoke scene draw callback
    if (drawFn)
        drawFn(cmdList);

    vkCmdEndRenderPass(cmdBuffer);
    // After vkCmdEndRenderPass, the render pass automatically transitions
    // the color image to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
#endif
}

