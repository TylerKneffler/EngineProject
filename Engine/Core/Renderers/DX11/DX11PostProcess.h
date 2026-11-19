#pragma once

#include "Core/Graphics/PostProcess.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

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
    // Scene depth for temporal anti-aliasing (single-sample, typeless SRV);
    // null leaves TAA off. Set before Compose each frame.
    void SetSceneDepth(ID3D11ShaderResourceView* depth) { m_sceneDepth = depth; }

private:
    void UploadConstants(ID3D11DeviceContext* context, const void* data);
    // Builds the bloom chain from source; leaves level 0 holding the sum.
    void RenderBloom(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source);
    // Meters source into the exposure target this frame writes; returns it.
    ID3D11ShaderResourceView* RenderExposure(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* source,
        const Engine::Graphics::PostProcessSettings& settings);
    void CreateExposureTargets(ID3D11Device* device);
    // Blends the scene with its reprojected history (TemporalResolvePS) into
    // the next history target, then copies the result into the scene colour
    // so bloom, exposure and compose read it unchanged.
    void RenderTemporal(ID3D11DeviceContext* context,
        const Engine::Graphics::PostProcessSettings& settings);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_resolvedColor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_accumulationColor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_resolvedRtv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_accumulationRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_accumulationSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bloom;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_bloomRtvs;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_bloomSrvs;
    uint32_t m_bloomLevels = 0;
    // Auto exposure: log-luminance map, histogram, two 1x1 exposure targets
    // alternating frames (see Graphics/PostProcess.h).
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_exposureMapRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_exposureMapSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_exposureHistogramRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_exposureHistogramSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_exposureRtvs[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_exposureSrvs[2];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_exposureLuminanceShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_exposureHistogramShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_exposureAdaptShader;
    Engine::Graphics::ExposureState m_exposure;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_bloomDownsampleShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_bloomUpsampleShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDisabled;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_additiveBlend;
    // TAA history: two targets alternating frames, created on first use.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_history[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_historyRtv[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_historySrv[2];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_temporalShader;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_sceneDepth;
    uint32_t m_historyIndex = 0;
    bool m_historyValid = false;
    uint32_t m_sampleCount = 1u;
    DXGI_FORMAT m_sceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
}
