#pragma once
#include "../IGameRenderer.h"
#include "DX11GraphicsProvider.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>

class DX11GameRenderer : public IGameRenderer
{
public:
    ~DX11GameRenderer() override;
    bool Init(void* hwnd, uint32_t width, uint32_t height) override;
    void Resize(uint32_t width, uint32_t height) override;
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void Clear(float r, float g, float b, float a = 1.0f) override;
    IGraphicsProvider* GetGraphicsProvider() override { return m_graphicsProvider.get(); }
    void BeginFrame() override;
    void EndFrame() override;
    std::unique_ptr<IGraphicsContext> CreateFrameGraphicsContext() override;

private:
    void CreateTargets();
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    std::unique_ptr<D3D11GraphicsProvider> m_graphicsProvider;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
