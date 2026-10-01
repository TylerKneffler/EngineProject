#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace Engine::Renderers
{
class DX11PostProcess
{
public:
    void Create(ID3D11Device* device, uint32_t width, uint32_t height,
        uint32_t sampleCount = 1u, uint32_t hdrBits = 16u);
    void BindScene(ID3D11DeviceContext* context,
        ID3D11DepthStencilView* depth, const float clearColor[4]);
    void ClearAccumulation(ID3D11DeviceContext* context);
    void AccumulateScene(ID3D11DeviceContext* context, float sampleWeight);
    void Compose(ID3D11DeviceContext* context,
        ID3D11RenderTargetView* output, bool useAccumulation = false);
    ID3D11Texture2D* GetSceneTexture() const { return m_sceneColor.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_resolvedColor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_accumulationColor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_resolvedRtv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_accumulationRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_accumulationSrv;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDisabled;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_additiveBlend;
    uint32_t m_sampleCount = 1u;
    DXGI_FORMAT m_sceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
}
