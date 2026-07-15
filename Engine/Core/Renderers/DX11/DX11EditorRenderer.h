#pragma once
#include "../IEditorRenderer.h"
#include "DX11GraphicsProvider.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>

class DX11EditorRenderer : public IEditorRenderer
{
public:
    ~DX11EditorRenderer() override;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override { return m_graphicsProvider.get(); }
    void MarkDirty() override { m_dirty = true; }
    void RenderIfNeeded(std::function<void()> drawFn = nullptr) override;
    std::pair<std::pair<void*, void*>, uint32_t> AllocateSrvSlot() override;
    void FreeSrvSlot(uint32_t slotIndex) override;
    bool CanAllocateSrvSlot() const override { return !m_freeSrvSlots.empty(); }
    uint32_t GetAvailableSrvSlots() const override { return static_cast<uint32_t>(m_freeSrvSlots.size()); }
    void* GetNativeDeviceHandle() const override { return m_device.Get(); }
    std::unique_ptr<IView> CreateViewBackend() override;
    void* GetCurrentCommandBuffer() const override { return m_context.Get(); }
    void* GetCurrentRenderTargetHandle() const override { return m_rtv.Get(); }

private:
    void CreateRenderTarget();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    std::unique_ptr<D3D11GraphicsProvider> m_graphicsProvider;
    std::vector<uint32_t> m_freeSrvSlots;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_dirty = true;
    bool m_imguiInitialized = false;
};
