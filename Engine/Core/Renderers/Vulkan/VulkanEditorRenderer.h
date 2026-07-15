#pragma once
#if defined(ENGINE_VULKAN_ENABLED)
#include "../IEditorRenderer.h"
#include "VulkanRenderCore.h"
#include "VulkanGraphicsProvider.h"
#include <vector>

class VulkanEditorRenderer : public IEditorRenderer
{
public:
    ~VulkanEditorRenderer() override;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override { return m_provider.get(); }
    void MarkDirty() override { m_dirty = true; }
    void RenderIfNeeded(std::function<void()> drawFn = nullptr) override;
    std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() override;
    void FreeSrvSlot(uint32_t slot) override;
    bool CanAllocateSrvSlot() const override { return !m_freeSlots.empty(); }
    uint32_t GetAvailableSrvSlots() const override { return static_cast<uint32_t>(m_freeSlots.size()); }
    void* GetNativeDeviceHandle() const override { return const_cast<VulkanViewDeviceContext*>(&m_viewContext); }
    std::unique_ptr<IView> CreateViewBackend() override;
    void* GetCurrentCommandBuffer() const override { return reinterpret_cast<void*>(m_commandBuffer); }
    void* GetCurrentRenderTargetHandle() const override { return nullptr; }
private:
    static PFN_vkVoidFunction LoadFunction(const char* name, void* instance);
    VulkanRenderCore m_core;
    VulkanViewDeviceContext m_viewContext{};
    std::unique_ptr<VulkanGraphicsProvider> m_provider;
    std::vector<uint32_t> m_freeSlots;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    float m_clearColor[4] = { 0.18f, 0.18f, 0.18f, 1.0f };
    uint32_t m_width = 0, m_height = 0;
    bool m_dirty = true;
    bool m_imguiInitialized = false;
};
#endif
