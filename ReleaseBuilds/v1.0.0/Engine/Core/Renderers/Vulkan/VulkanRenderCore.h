#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanCommon.h"
#include <Windows.h>
#include <vector>

class VulkanRenderCore
{
public:
    ~VulkanRenderCore();
    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool mainDepth);
    void Resize(uint32_t width, uint32_t height);
    VkCommandBuffer BeginFrame();
    void EndFrame();
    void WaitIdle() const;

    VkInstance GetInstance() const { return m_instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    VkDevice GetDevice() const { return m_device; }
    VkQueue GetQueue() const { return m_queue; }
    uint32_t GetQueueFamily() const { return m_queueFamily; }
    VkRenderPass GetMainRenderPass() const { return m_mainRenderPass; }
    VkRenderPass GetOffscreenRenderPass() const { return m_offscreenRenderPass; }
    VkFramebuffer GetCurrentFramebuffer() const { return m_framebuffers[m_imageIndex]; }
    VkCommandBuffer GetCurrentCommandBuffer() const { return m_currentCommandBuffer; }
    VkExtent2D GetExtent() const { return m_extent; }
    uint32_t GetImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    uint32_t GetMinImageCount() const { return m_minImageCount; }
    VkFormat GetSwapchainFormat() const { return m_surfaceFormat.format; }

private:
    void CreateInstance();
    void SelectDevice();
    void CreateDevice();
    void CreateCommandResources();
    void CreateSyncResources();
    void CreateSwapchain();
    void DestroySwapchain();
    VkRenderPass CreateRenderPass(VkFormat colorFormat, bool depth, VkImageLayout finalLayout);

    HWND m_hwnd = nullptr;
    uint32_t m_requestedWidth = 0;
    uint32_t m_requestedHeight = 0;
    bool m_mainDepth = false;
    bool m_resizePending = false;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = UINT32_MAX;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR m_surfaceFormat{};
    VkExtent2D m_extent{};
    uint32_t m_minImageCount = 2;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
    std::vector<VulkanImageResource> m_depthImages;
    std::vector<VkFramebuffer> m_framebuffers;
    VkRenderPass m_mainRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_offscreenRenderPass = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    static constexpr uint32_t FRAME_COUNT = 2;
    VkCommandBuffer m_commandBuffers[FRAME_COUNT]{};
    VkSemaphore m_imageAvailable[FRAME_COUNT]{};
    VkSemaphore m_renderFinished[FRAME_COUNT]{};
    VkFence m_inFlight[FRAME_COUNT]{};
    uint32_t m_frameIndex = 0;
    uint32_t m_imageIndex = 0;
    VkCommandBuffer m_currentCommandBuffer = VK_NULL_HANDLE;
};
#endif
