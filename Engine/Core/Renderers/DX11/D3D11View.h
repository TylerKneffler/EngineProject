#pragma once
#include "../IView.h"
#include <wrl/client.h>
#include <d3d11.h>

class D3D11View : public IView
{
public:
    void Init(void* device, uint32_t width, uint32_t height,
              void* srvCpu, void* srvGpu, uint32_t srvSlotIndex = 0) override;
    void Resize(void* device, uint32_t width, uint32_t height) override;
    void Render(void* context, void* mainRtv,
                std::function<void(void*)> drawFn = nullptr) override;
    float GetAspect() const override { return m_aspect; }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void* GetImGuiTextureHandle() const override { return m_srv.Get(); }
    uint32_t GetSrvSlotIndex() const override { return m_srvSlotIndex; }

private:
    void CreateResources(ID3D11Device* device, uint32_t width, uint32_t height);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
    uint32_t m_srvSlotIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_aspect = 1.0f;
};
