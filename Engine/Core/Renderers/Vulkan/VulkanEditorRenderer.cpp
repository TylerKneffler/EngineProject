#include "pch.h"
#include "VulkanEditorRenderer.h"
#include "VulkanGraphicsProvider.h"
#include "VulkanView.h"

#if defined(ENGINE_VULKAN_ENABLED)
#include <array>
#include <set>

namespace
{
struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

bool CheckVk(VkResult result, const char* msg)
{
    if (result != VK_SUCCESS)
    {
        std::string out = std::string("[VulkanEditorRenderer] ") + msg + "\n";
        OutputDebugStringA(out.c_str());
        return false;
    }
    return true;
}

bool FindQueueFamilies(VkPhysicalDevice device,
                       VkSurfaceKHR surface,
                       uint32_t& outGraphicsFamily,
                       uint32_t& outPresentFamily)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0)
        return false;

    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, families.data());

    bool foundGraphics = false;
    bool foundPresent = false;
    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            outGraphicsFamily = i;
            foundGraphics = true;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (families[i].queueCount > 0 && presentSupport)
        {
            outPresentFamily = i;
            foundPresent = true;
        }

        if (foundGraphics && foundPresent)
            return true;
    }

    return false;
}

SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount > 0)
    {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount > 0)
    {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const auto& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return format;
        }
    }
    return formats.empty()
        ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
        : formats[0];
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes)
{
    for (auto mode : presentModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
        return capabilities.currentExtent;

    VkExtent2D actualExtent{ width, height };
    actualExtent.width = std::max(capabilities.minImageExtent.width,
                                  std::min(capabilities.maxImageExtent.width, actualExtent.width));
    actualExtent.height = std::max(capabilities.minImageExtent.height,
                                   std::min(capabilities.maxImageExtent.height, actualExtent.height));
    return actualExtent;
}
}
#endif

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
VulkanEditorRenderer::VulkanEditorRenderer()
{
    // Initialize SRV free list (slot 0 reserved for ImGui font atlas)
    for (uint32_t i = MAX_SRV_SLOTS - 1; i >= 1; --i)
        m_freeSrvSlots.push_back(i);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
VulkanEditorRenderer::~VulkanEditorRenderer()
{
#if defined(ENGINE_VULKAN_ENABLED)
    if (m_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(m_device);

        for (VkSemaphore sem : m_renderFinishedSemaphores)
            if (sem != VK_NULL_HANDLE) vkDestroySemaphore(m_device, sem, nullptr);
        for (VkSemaphore sem : m_imageAvailableSemaphores)
            if (sem != VK_NULL_HANDLE) vkDestroySemaphore(m_device, sem, nullptr);
        for (VkFence fence : m_inFlightFences)
            if (fence != VK_NULL_HANDLE) vkDestroyFence(m_device, fence, nullptr);
        for (VkImageView view : m_swapchainImageViews)
            if (view != VK_NULL_HANDLE) vkDestroyImageView(m_device, view, nullptr);

        if (m_commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        if (m_swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);

        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance != VK_NULL_HANDLE)
        vkDestroyInstance(m_instance, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool VulkanEditorRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
#if !defined(ENGINE_VULKAN_ENABLED)
    (void)hwnd;
    m_width = width;
    m_height = height;
    m_currentFrame = 0;
    m_currentImageIndex = 0;

    OutputDebugStringA("[VulkanEditorRenderer] Vulkan SDK support is not enabled in this build\n");
    return false;
#else
    if (!hwnd)
        return false;

    m_width = width;
    m_height = height;
    m_currentFrame = 0;
    m_currentImageIndex = 0;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "EngineProject Editor";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName = "Engine";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(instanceExtensions));
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;

    if (!CheckVk(vkCreateInstance(&instanceInfo, nullptr, &m_instance), "Failed to create Vulkan instance"))
        return false;

    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(hwnd);
    if (!CheckVk(vkCreateWin32SurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface), "Failed to create Win32 surface"))
        return false;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0)
        return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    bool found = false;
    for (VkPhysicalDevice dev : devices)
    {
        uint32_t graphicsFamily = 0;
        uint32_t presentFamily = 0;
        if (FindQueueFamilies(dev, m_surface, graphicsFamily, presentFamily))
        {
            auto support = QuerySwapchainSupport(dev, m_surface);
            if (!support.formats.empty() && !support.presentModes.empty())
            {
                m_physicalDevice = dev;
                m_graphicsQueueFamilyIndex = graphicsFamily;
                m_presentQueueFamilyIndex = presentFamily;
                found = true;
                break;
            }
        }
    }

    if (!found)
        return false;

    std::set<uint32_t> uniqueFamilies = {
        m_graphicsQueueFamilyIndex,
        m_presentQueueFamilyIndex
    };

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &queuePriority;
        queueInfos.push_back(qi);
    }

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(deviceExtensions));
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    if (!CheckVk(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device), "Failed to create Vulkan logical device"))
        return false;

    vkGetDeviceQueue(m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentQueueFamilyIndex, 0, &m_presentQueue);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!CheckVk(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "Failed to create command pool"))
        return false;

    auto support = QuerySwapchainSupport(m_physicalDevice, m_surface);
    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
    VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
    VkExtent2D extent = ChooseExtent(support.capabilities, width, height);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
        imageCount = support.capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = m_surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndices[] = { m_graphicsQueueFamilyIndex, m_presentQueueFamilyIndex };
    if (m_graphicsQueueFamilyIndex != m_presentQueueFamilyIndex)
    {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.queueFamilyIndexCount = 2;
        swapchainInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    swapchainInfo.preTransform = support.capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    if (!CheckVk(vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &m_swapchain), "Failed to create swapchain"))
        return false;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

    m_swapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < m_swapchainImages.size(); ++i)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat.format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (!CheckVk(vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainImageViews[i]), "Failed to create image view"))
            return false;
    }

    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    if (!CheckVk(vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()), "Failed to allocate command buffers"))
        return false;

    m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        if (!CheckVk(vkCreateSemaphore(m_device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]), "Failed to create image-available semaphore"))
            return false;
        if (!CheckVk(vkCreateSemaphore(m_device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]), "Failed to create render-finished semaphore"))
            return false;
        if (!CheckVk(vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]), "Failed to create in-flight fence"))
            return false;
    }

    // Create graphics provider
    m_graphicsProvider = std::make_unique<VulkanGraphicsProvider>(
        m_device, m_graphicsQueue, m_commandPool);

    return m_graphicsProvider != nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    
    m_width = width;
    m_height = height;
    MarkDirty();

    // TODO: Recreate swapchain and framebuffers
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::Clear(float r, float g, float b, float a)
{
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
    MarkDirty();
    // TODO: Record clear command for next frame
}

// ---------------------------------------------------------------------------
// GetGraphicsProvider
// ---------------------------------------------------------------------------
IGraphicsProvider* VulkanEditorRenderer::GetGraphicsProvider()
{
    return m_graphicsProvider.get();
}

// ---------------------------------------------------------------------------
// MarkDirty
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::MarkDirty()
{
    m_isDirty = true;
}

// ---------------------------------------------------------------------------
// RenderIfNeeded
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::RenderIfNeeded(std::function<void()> drawFn)
{
    if (!m_isDirty) return;

#if defined(ENGINE_VULKAN_ENABLED)
    if (m_device != VK_NULL_HANDLE && m_swapchain != VK_NULL_HANDLE)
    {
        vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

        VkResult acquireResult = vkAcquireNextImageKHR(
            m_device,
            m_swapchain,
            UINT64_MAX,
            m_imageAvailableSemaphores[m_currentFrame],
            VK_NULL_HANDLE,
            &m_currentImageIndex);

        if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)
        {
            vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo);

            if (drawFn)
                drawFn();

            vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);

            VkSemaphore waitSemaphores[] = { m_imageAvailableSemaphores[m_currentFrame] };
            VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
            VkSemaphore signalSemaphores[] = { m_renderFinishedSemaphores[m_currentFrame] };

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = waitSemaphores;
            submitInfo.pWaitDstStageMask = waitStages;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = signalSemaphores;

            if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]) != VK_SUCCESS)
                OutputDebugStringA("[VulkanEditorRenderer] Queue submit failed\n");

            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = signalSemaphores;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &m_swapchain;
            presentInfo.pImageIndices = &m_currentImageIndex;

            VkResult presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
            if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR && presentResult != VK_ERROR_OUT_OF_DATE_KHR)
                OutputDebugStringA("[VulkanEditorRenderer] Queue present failed\n");
        }
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
#else
    (void)drawFn;
#endif

    m_isDirty = false;
}

// ---------------------------------------------------------------------------
// AllocateSrvSlot
// ---------------------------------------------------------------------------
std::pair<std::pair<void*, void*>, uint32_t> VulkanEditorRenderer::AllocateSrvSlot()
{
    if (m_freeSrvSlots.empty())
        return {{nullptr, nullptr}, 0};

    uint32_t slotIndex = m_freeSrvSlots.back();
    m_freeSrvSlots.pop_back();

    // TODO: Return actual descriptor handles for the allocated slot
    return {{nullptr, nullptr}, slotIndex};
}

// ---------------------------------------------------------------------------
// FreeSrvSlot
// ---------------------------------------------------------------------------
void VulkanEditorRenderer::FreeSrvSlot(uint32_t slotIndex)
{
    m_freeSrvSlots.push_back(slotIndex);
}

// ---------------------------------------------------------------------------
// CanAllocateSrvSlot
// ---------------------------------------------------------------------------
bool VulkanEditorRenderer::CanAllocateSrvSlot() const
{
    return !m_freeSrvSlots.empty();
}

// ---------------------------------------------------------------------------
// GetAvailableSrvSlots
// ---------------------------------------------------------------------------
uint32_t VulkanEditorRenderer::GetAvailableSrvSlots() const
{
    return static_cast<uint32_t>(m_freeSrvSlots.size());
}

std::unique_ptr<IView> VulkanEditorRenderer::CreateViewBackend()
{
    return std::make_unique<VulkanView>();
}
