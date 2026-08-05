#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "../IGameRenderer.h"
#include "VulkanRenderCore.h"
#include "VulkanGraphicsProvider.h"

class VulkanGameRenderer : public IGameRenderer
{
public:
    ~VulkanGameRenderer() override = default;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override { return m_provider.get(); }
    void BeginFrame() override;
    void EndFrame() override;
    std::unique_ptr<IGraphicsContext> CreateFrameGraphicsContext() override;
private:
    void BeginMainRenderPass(const float color[4]);
    VulkanRenderCore m_core;
    std::unique_ptr<VulkanGraphicsProvider> m_provider;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
    bool m_renderPassActive = false;
};
#endif
