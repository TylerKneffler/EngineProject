#pragma once
#include "../IEditorRenderer.h"
#include "VulkanGraphicsProvider.h"
#include <functional>

#if defined(ENGINE_VULKAN_ENABLED)
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR 1
    #endif
    #include <vulkan/vulkan.h>
#else
    // Minimal Vulkan type stubs for compilation when Vulkan SDK is not installed
    typedef void* VkInstance;
    typedef void* VkDevice;
    typedef void* VkPhysicalDevice;
    typedef void* VkQueue;
    typedef void* VkCommandPool;
    typedef void* VkSurfaceKHR;
    typedef void* VkSwapchainKHR;
    typedef void* VkImage;
    typedef void* VkImageView;
    typedef void* VkFramebuffer;
    typedef void* VkRenderPass;
    typedef void* VkFence;
    typedef void* VkSemaphore;
    typedef void* VkCommandBuffer;
    typedef void* VkDescriptorHeap;
    
    #define VK_NULL_HANDLE nullptr
#endif

#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// VulkanEditorRenderer — Vulkan implementation of IEditorRenderer
//
// Manages the editor rendering pipeline using Vulkan.
// Provides dirty-driven rendering and SRV slot management for editor views.
// ---------------------------------------------------------------------------
class VulkanEditorRenderer : public IEditorRenderer
{
public:
    VulkanEditorRenderer();
    virtual ~VulkanEditorRenderer();

    // IRenderer interface
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override;

    // IEditorRenderer interface
    void MarkDirty() override;
    void RenderIfNeeded(std::function<void()> drawFn = nullptr) override;
    std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() override;
    void FreeSrvSlot(uint32_t slotIndex) override;
    bool CanAllocateSrvSlot() const override;
    uint32_t GetAvailableSrvSlots() const override;
    void* GetNativeDeviceHandle() const override { return m_device; }
    std::unique_ptr<IView> CreateViewBackend() override;

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;

    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;

    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkFence> m_inFlightFences;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_currentFrame = 0;
    uint32_t m_currentImageIndex = 0;
    uint32_t m_graphicsQueueFamilyIndex = 0;
    uint32_t m_presentQueueFamilyIndex = 0;
    float m_clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    bool m_isDirty = true;
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    // SRV slot management (for editor views)
    std::vector<uint32_t> m_freeSrvSlots;
    static constexpr uint32_t MAX_SRV_SLOTS = 32;

    std::unique_ptr<VulkanGraphicsProvider> m_graphicsProvider;
};
