#include "pch.h"
#include "VulkanView.h"

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
    m_device = static_cast<VkDevice>(device);
    m_srvSlotIndex = srvSlotIndex;
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
void VulkanView::CreateResources(void* device, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;
    m_aspect = static_cast<float>(width) / static_cast<float>(height);

    // TODO: Implement Vulkan image creation
    // - Create VkImage for color target
    // - Create VkImage for depth buffer
    // - Create corresponding VkImageView objects
    // - Create VkRenderPass
    // - Create VkFramebuffer
    // - Register SRV descriptor set for ImGui
}

// ---------------------------------------------------------------------------
// CleanupResources
// ---------------------------------------------------------------------------
void VulkanView::CleanupResources()
{
    if (!m_device) return;

    // TODO: Implement resource cleanup
    // - Destroy VkImageView objects
    // - Destroy VkImage objects
    // - Free VkDeviceMemory allocations
    // - Destroy VkFramebuffer
    // - Destroy VkRenderPass
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void VulkanView::Render(void* cmdList, void* mainRtv,
                        std::function<void(void*)> drawFn)
{
    if (!m_device || !m_framebuffer) return;

    auto* cmdBuffer = static_cast<VkCommandBuffer>(cmdList);

    // TODO: Implement Vulkan rendering pipeline
    // - Begin command buffer recording
    // - Transition color image to RENDER_TARGET_OPTIMAL
    // - Begin render pass
    // - Set viewport and scissor
    // - Clear color and depth
    // - Invoke drawFn callback
    // - End render pass
    // - Transition color image back to SHADER_READ_ONLY_OPTIMAL
    // - Restore main render target

    if (drawFn)
        drawFn(cmdList);
}
