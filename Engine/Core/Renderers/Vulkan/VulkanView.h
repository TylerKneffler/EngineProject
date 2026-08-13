#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "../IView.h"
#include "VulkanCommon.h"

namespace Engine::Renderers
{
class VulkanView : public IView
{
public:
    ~VulkanView() override;
    void Init(void* deviceContext, uint32_t width, uint32_t height,
              void* srvCpu, void* srvGpu, uint32_t slot = 0) override;
    void Resize(void* deviceContext, uint32_t width, uint32_t height) override;
    void Render(void* commandBuffer, void* mainTarget,
                std::function<void(void*)> drawFn = nullptr) override;
    void SetClearColor(float r, float g, float b, float a = 1.0f) override
    {
        m_clearColor[0] = r; m_clearColor[1] = g;
        m_clearColor[2] = b; m_clearColor[3] = a;
    }
    float GetAspect() const override { return m_aspect; }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void* GetUiTextureHandle() const override { return m_uiTexture; }
    uint32_t GetSrvSlotIndex() const override { return m_slot; }
private:
    void CreateResources(uint32_t width, uint32_t height);
    void DestroyResources();
    VulkanViewDeviceContext m_context{};
    VulkanImageResource m_color;
    VulkanImageResource m_depth;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    void* m_uiTexture = nullptr;
    uint32_t m_width = 0, m_height = 0, m_slot = 0;
    float m_aspect = 1.0f;
    float m_clearColor[4] = { 0.0f, 0.0f, 0.502f, 1.0f };
};
}
#endif
