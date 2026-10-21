#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGameRenderer.h"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace Engine::Renderers
{
VulkanGameRenderer::~VulkanGameRenderer()
{
    // Provider-owned query pools, textures and buffers are destroyed before
    // VulkanRenderCore because of member order. Finish their GPU use first.
    m_core.WaitIdle();
    DestroySceneCapture();
}

bool VulkanGameRenderer::Init(void* hwnd, uint32_t width, uint32_t height)
{
    m_width = width; m_height = height;
    if (!m_core.Init(static_cast<HWND>(hwnd), width, height, true)) return false;
    m_provider = std::make_unique<VulkanGraphicsProvider>(
        m_core.GetPhysicalDevice(), m_core.GetDevice(), m_core.GetOffscreenRenderPass(),
        m_core.GetQueue(), m_core.GetQueueFamily());
    m_postProcess.Init(m_core.GetPhysicalDevice(), m_core.GetDevice(),
        m_core.GetOffscreenRenderPass(), m_core.GetMainRenderPass(), width, height);
    return true;
}

void VulkanGameRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width && height)
    {
        m_core.WaitIdle();
        m_width = width; m_height = height;
        m_core.Resize(width, height);
        m_postProcess.Resize(width, height);
        DestroySceneCapture();
    }
}

void VulkanGameRenderer::BeginFrame()
{
    m_commandBuffer = m_core.BeginFrame();
    m_renderPassActive = false;
    if (m_commandBuffer && m_provider)
    {
        auto* factory = m_provider->GetContextFactory();
        factory->SetCommandBuffer(reinterpret_cast<void*>(m_commandBuffer));
        factory->PrepareFrame(m_core.GetFrameIndex());
    }
}

void VulkanGameRenderer::BeginMainRenderPass(const float color[4])
{
    if (!m_commandBuffer || m_renderPassActive) return;
    memcpy(m_clearColor, color, sizeof(m_clearColor));
    m_postProcess.BeginScene(m_commandBuffer, m_clearColor);
    m_renderPassActive = true;
}

void VulkanGameRenderer::Clear(float r, float g, float b, float a)
{ const float color[] = { r, g, b, a }; BeginMainRenderPass(color); }

void VulkanGameRenderer::EndFrame()
{
    if (!m_commandBuffer) return;
    if (!m_renderPassActive) { const float color[] = { 0.1f, 0.1f, 0.1f, 1.0f }; BeginMainRenderPass(color); }
    if (m_provider && m_provider->GetContextFactory())
    {
        auto* factory = m_provider->GetContextFactory();
        factory->FinalizeFrame();
        m_frameTelemetry = factory->GetFrameTimingTelemetry();
    }
    m_postProcess.EndScene(m_commandBuffer);
    if (m_captureRequested) RecordSceneCapture();
    const auto extent = m_core.GetExtent();
    m_postProcess.BeginComposition(m_commandBuffer,
        m_core.GetCurrentFramebuffer(), extent.width, extent.height, m_clearColor);
    m_postProcess.EndComposition(m_commandBuffer);
    const auto presentationStart = std::chrono::steady_clock::now();
    m_core.EndFrame();
    m_frameTelemetry.cpuPresentationMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - presentationStart).count();
    // DXGI's flip-model designation does not apply to Vulkan swap chains.
    m_frameTelemetry.flipModelSwapChain = false;
    m_commandBuffer = VK_NULL_HANDLE; m_renderPassActive = false;
}

void VulkanGameRenderer::DestroySceneCapture()
{
    const VkDevice device = m_core.GetDevice();
    if (device && m_captureBuffer) vkDestroyBuffer(device, m_captureBuffer, nullptr);
    if (device && m_captureMemory) vkFreeMemory(device, m_captureMemory, nullptr);
    m_captureBuffer = VK_NULL_HANDLE;
    m_captureMemory = VK_NULL_HANDLE;
}

void VulkanGameRenderer::RecordSceneCapture()
{
    const VkDevice device = m_core.GetDevice();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_width) * m_height * 8;
    if (!m_captureBuffer)
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(device, &info, nullptr, &m_captureBuffer),
            "vkCreateBuffer(scene capture)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, m_captureBuffer, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = VulkanFindMemoryType(
            m_core.GetPhysicalDevice(), requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkCheck(vkAllocateMemory(device, &allocation, nullptr,
            &m_captureMemory), "vkAllocateMemory(scene capture)");
        VkCheck(vkBindBufferMemory(device, m_captureBuffer,
            m_captureMemory, 0), "vkBindBufferMemory(scene capture)");
    }
    VkImageMemoryBarrier before{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    before.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.image = m_postProcess.GetSceneColor();
    before.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    before.subresourceRange.levelCount = 1;
    before.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(m_commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &before);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyImageToBuffer(m_commandBuffer, before.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_captureBuffer, 1, &region);
    VkImageMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &after);
    VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = m_captureBuffer;
    host.size = bytes;
    vkCmdPipelineBarrier(m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    m_captureRequested = false;
}

bool VulkanGameRenderer::ReadSceneCaptureRGBA(std::vector<uint8_t>& pixels)
{
    if (!m_captureMemory) return false;
    void* mapped = nullptr;
    if (vkMapMemory(m_core.GetDevice(), m_captureMemory, 0,
        VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped) return false;
    const auto halfToByte = [](uint16_t bits)
    {
        const int exponent = (bits >> 10) & 31;
        const float value = exponent == 0
            ? std::ldexp(static_cast<float>(bits & 1023), -24)
            : (exponent == 31 ? 1.f : std::ldexp(
                1.f + static_cast<float>(bits & 1023) / 1024.f,
                exponent - 15));
        return static_cast<uint8_t>(std::round(std::pow(
            std::clamp(value, 0.f, 1.f), 1.f / 2.2f) * 255.f));
    };
    const auto* values = static_cast<const uint16_t*>(mapped);
    pixels.resize(static_cast<size_t>(m_width) * m_height * 4);
    for (size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = halfToByte(values[index]);
    vkUnmapMemory(m_core.GetDevice(), m_captureMemory);
    return true;
}

std::unique_ptr<Engine::Graphics::IGraphicsContext> VulkanGameRenderer::CreateFrameGraphicsContext()
{
    if (!m_commandBuffer || !m_provider)
        return nullptr;
    auto* factory = m_provider->GetContextFactory();
    factory->SetCommandBuffer(reinterpret_cast<void*>(m_commandBuffer));
    return factory->CreateContext();
}
}
#endif
