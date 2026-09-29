#include "pch.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "VulkanGameRenderer.h"
#include <chrono>

namespace Engine::Renderers
{
VulkanGameRenderer::~VulkanGameRenderer()
{
    // Provider-owned query pools, textures and buffers are destroyed before
    // VulkanRenderCore because of member order. Finish their GPU use first.
    m_core.WaitIdle();
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
