#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanRenderCore.h"
#include <algorithm>
#include <cstring>

VulkanRenderCore::~VulkanRenderCore()
{
    if (!m_instance) return;
    if (m_device) vkDeviceWaitIdle(m_device);
    DestroySwapchain();
    if (m_offscreenRenderPass) vkDestroyRenderPass(m_device, m_offscreenRenderPass, nullptr);
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        if (m_imageAvailable[i]) vkDestroySemaphore(m_device, m_imageAvailable[i], nullptr);
        if (m_renderFinished[i]) vkDestroySemaphore(m_device, m_renderFinished[i], nullptr);
        if (m_inFlight[i]) vkDestroyFence(m_device, m_inFlight[i], nullptr);
    }
    if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    if (m_device) vkDestroyDevice(m_device, nullptr);
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

bool VulkanRenderCore::Init(HWND hwnd, uint32_t width, uint32_t height, bool mainDepth)
{
    try
    {
        m_hwnd = hwnd;
        m_requestedWidth = width;
        m_requestedHeight = height;
        m_mainDepth = mainDepth;
        VkCheck(volkInitialize(), "volkInitialize");
        CreateInstance();
        volkLoadInstance(m_instance);
        VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = hwnd;
        VkCheck(vkCreateWin32SurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface), "vkCreateWin32SurfaceKHR");
        SelectDevice();
        CreateDevice();
        volkLoadDevice(m_device);
        CreateCommandResources();
        CreateSyncResources();
        m_offscreenRenderPass = CreateRenderPass(
            VK_FORMAT_R8G8B8A8_UNORM, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CreateSwapchain();
        return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[Vulkan] Initialization failed: ") + error.what() + "\n").c_str());
        return false;
    }
}

void VulkanRenderCore::CreateInstance()
{
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "EngineProject";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "EngineProject";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_1;
    const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    createInfo.pApplicationInfo = &app;
    createInfo.enabledExtensionCount = ARRAYSIZE(extensions);
    createInfo.ppEnabledExtensionNames = extensions;
    VkCheck(vkCreateInstance(&createInfo, nullptr, &m_instance), "vkCreateInstance");
}

void VulkanRenderCore::SelectDevice()
{
    uint32_t deviceCount = 0;
    VkCheck(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
    if (!deviceCount) throw std::runtime_error("No Vulkan physical devices were found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    VkCheck(vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = -1;
    for (VkPhysicalDevice device : devices)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
            bool swapchain = std::any_of(extensions.begin(), extensions.end(), [](const auto& ext)
            { return std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; });
            if (!swapchain) continue;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore)
            {
                bestScore = score;
                m_physicalDevice = device;
                m_queueFamily = i;
            }
        }
    }
    if (!m_physicalDevice) throw std::runtime_error("No Vulkan graphics/present queue with swapchain support was found");
}

void VulkanRenderCore::CreateDevice()
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = ARRAYSIZE(extensions);
    createInfo.ppEnabledExtensionNames = extensions;
    VkCheck(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
}

void VulkanRenderCore::CreateCommandResources()
{
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    VkCheck(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocate.commandPool = m_commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = FRAME_COUNT;
    VkCheck(vkAllocateCommandBuffers(m_device, &allocate, m_commandBuffers), "vkAllocateCommandBuffers");
}

void VulkanRenderCore::CreateSyncResources()
{
    VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < FRAME_COUNT; ++i)
    {
        VkCheck(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailable[i]), "vkCreateSemaphore");
        VkCheck(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinished[i]), "vkCreateSemaphore");
        VkCheck(vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlight[i]), "vkCreateFence");
    }
}

VkRenderPass VulkanRenderCore::CreateRenderPass(VkFormat colorFormat, bool depth, VkImageLayout finalLayout)
{
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = finalLayout;
    VkAttachmentReference colorReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthReference{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    if (depth)
    {
        attachments[1].format = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = depth ? &depthReference : nullptr;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo createInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    createInfo.attachmentCount = depth ? 2u : 1u;
    createInfo.pAttachments = attachments;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = ARRAYSIZE(dependencies);
    createInfo.pDependencies = dependencies;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCheck(vkCreateRenderPass(m_device, &createInfo, nullptr, &renderPass), "vkCreateRenderPass");
    return renderPass;
}

void VulkanRenderCore::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR capabilities{};
    VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());
    if (formats.empty()) throw std::runtime_error("The Vulkan surface exposes no formats");
    m_surfaceFormat = formats[0];
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            m_surfaceFormat = format;
    m_extent = capabilities.currentExtent.width != UINT32_MAX
        ? capabilities.currentExtent
        : VkExtent2D{ std::clamp(m_requestedWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                      std::clamp(m_requestedHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height) };
    m_minImageCount = std::max(2u, capabilities.minImageCount);
    uint32_t imageCount = m_minImageCount + 1;
    if (capabilities.maxImageCount && imageCount > capabilities.maxImageCount) imageCount = capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = m_surfaceFormat.format;
    createInfo.imageColorSpace = m_surfaceFormat.colorSpace;
    createInfo.imageExtent = m_extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    VkCheck(vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain), "vkCreateSwapchainKHR");
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_images.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_images.data());

    m_mainRenderPass = CreateRenderPass(m_surfaceFormat.format, m_mainDepth, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    m_imageViews.resize(imageCount);
    m_framebuffers.resize(imageCount);
    if (m_mainDepth) m_depthImages.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = m_images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_surfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkCheck(vkCreateImageView(m_device, &viewInfo, nullptr, &m_imageViews[i]), "vkCreateImageView(swapchain)");
        VkImageView attachments[2] = { m_imageViews[i], VK_NULL_HANDLE };
        if (m_mainDepth)
        {
            m_depthImages[i] = VulkanCreateImage(m_physicalDevice, m_device, m_extent.width, m_extent.height,
                VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            attachments[1] = m_depthImages[i].view;
        }
        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = m_mainRenderPass;
        framebufferInfo.attachmentCount = m_mainDepth ? 2u : 1u;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = m_extent.width;
        framebufferInfo.height = m_extent.height;
        framebufferInfo.layers = 1;
        VkCheck(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]), "vkCreateFramebuffer");
    }
}

void VulkanRenderCore::DestroySwapchain()
{
    if (!m_device) return;
    for (auto framebuffer : m_framebuffers) if (framebuffer) vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    for (auto& depth : m_depthImages) VulkanDestroyImage(m_device, depth);
    for (auto view : m_imageViews) if (view) vkDestroyImageView(m_device, view, nullptr);
    if (m_mainRenderPass) vkDestroyRenderPass(m_device, m_mainRenderPass, nullptr);
    if (m_swapchain) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_framebuffers.clear(); m_depthImages.clear(); m_imageViews.clear(); m_images.clear();
    m_mainRenderPass = VK_NULL_HANDLE; m_swapchain = VK_NULL_HANDLE;
}

void VulkanRenderCore::Resize(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    m_requestedWidth = width; m_requestedHeight = height; m_resizePending = true;
}

VkCommandBuffer VulkanRenderCore::BeginFrame()
{
    if (m_resizePending)
    {
        vkDeviceWaitIdle(m_device); DestroySwapchain(); CreateSwapchain(); m_resizePending = false;
    }
    VkCheck(vkWaitForFences(m_device, 1, &m_inFlight[m_frameIndex], VK_TRUE, UINT64_MAX), "vkWaitForFences");
    VkResult acquired = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_imageAvailable[m_frameIndex], VK_NULL_HANDLE, &m_imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { m_resizePending = true; return VK_NULL_HANDLE; }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) VkCheck(acquired, "vkAcquireNextImageKHR");
    VkCheck(vkResetFences(m_device, 1, &m_inFlight[m_frameIndex]), "vkResetFences");
    m_currentCommandBuffer = m_commandBuffers[m_frameIndex];
    VkCheck(vkResetCommandBuffer(m_currentCommandBuffer, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCheck(vkBeginCommandBuffer(m_currentCommandBuffer, &begin), "vkBeginCommandBuffer");
    return m_currentCommandBuffer;
}

void VulkanRenderCore::EndFrame()
{
    if (!m_currentCommandBuffer) return;
    VkCheck(vkEndCommandBuffer(m_currentCommandBuffer), "vkEndCommandBuffer");
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &m_imageAvailable[m_frameIndex];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &m_currentCommandBuffer;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &m_renderFinished[m_frameIndex];
    VkCheck(vkQueueSubmit(m_queue, 1, &submit, m_inFlight[m_frameIndex]), "vkQueueSubmit");
    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &m_renderFinished[m_frameIndex];
    present.swapchainCount = 1; present.pSwapchains = &m_swapchain; present.pImageIndices = &m_imageIndex;
    VkResult result = vkQueuePresentKHR(m_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) m_resizePending = true;
    else VkCheck(result, "vkQueuePresentKHR");
    m_currentCommandBuffer = VK_NULL_HANDLE;
    m_frameIndex = (m_frameIndex + 1) % FRAME_COUNT;
}

void VulkanRenderCore::WaitIdle() const { if (m_device) vkDeviceWaitIdle(m_device); }
#endif
