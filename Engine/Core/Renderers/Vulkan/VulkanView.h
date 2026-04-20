#pragma once
#include "../IView.h"

#ifdef VK_USE_PLATFORM_WIN32_KHR
    #include <vulkan/vulkan.h>
#else
    // Minimal Vulkan type stubs for compilation when Vulkan SDK is not installed
    typedef void* VkDevice;
    typedef void* VkPhysicalDevice;
    typedef void* VkImage;
    typedef void* VkImageView;
    typedef void* VkDeviceMemory;
    typedef void* VkRenderPass;
    typedef void* VkFramebuffer;
    typedef void* VkCommandBuffer;
    typedef void* VkDescriptorSet;
    typedef void* VkAllocationCallbacks;
    
    #define VK_NULL_HANDLE nullptr
#endif

#include <functional>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// VulkanView — Vulkan implementation of IView
//
// Owns offscreen render target and depth buffer resources for editor views.
// Creates Vulkan images, image views, and framebuffers for rendering.
// ---------------------------------------------------------------------------
class VulkanView : public IView
{
public:
    VulkanView() = default;
    virtual ~VulkanView();

    void Init(void* device,
              uint32_t width, uint32_t height,
              void* srvCpu, void* srvGpu,
              uint32_t srvSlotIndex = 0) override;

    void Resize(void* device, uint32_t width, uint32_t height) override;

    void Render(void* cmdList, void* mainRtv,
                std::function<void(void*)> drawFn = nullptr) override;

    float    GetAspect() const override { return m_aspect; }
    uint32_t GetWidth()  const override { return m_width;  }
    uint32_t GetHeight() const override { return m_height; }

    uint32_t GetSrvSlotIndex() const override { return m_srvSlotIndex; }

    // Vulkan-specific accessors
    VkImage GetColorImage() const { return m_colorImage; }
    VkImageView GetColorImageView() const { return m_colorImageView; }
    VkImage GetDepthImage() const { return m_depthImage; }
    VkImageView GetDepthImageView() const { return m_depthImageView; }
    VkFramebuffer GetFramebuffer() const { return m_framebuffer; }

private:
    void CreateResources(void* device, uint32_t width, uint32_t height);
    void CleanupResources();

    // Vulkan device and memory allocator
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkAllocationCallbacks* m_allocator = nullptr;

    // Color target resources
    VkImage m_colorImage = VK_NULL_HANDLE;
    VkImageView m_colorImageView = VK_NULL_HANDLE;
    VkDeviceMemory m_colorMemory = VK_NULL_HANDLE;

    // Depth buffer resources
    VkImage m_depthImage = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;

    // Framebuffer and render pass
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

    // SRV descriptor set (for ImGui)
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    // Dimensions and aspect ratio
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    float    m_aspect = 1.0f;

    // Slot tracking for resource cleanup
    uint32_t m_srvSlotIndex = 0;
};
