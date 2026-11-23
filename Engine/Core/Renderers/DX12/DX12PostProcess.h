#pragma once

#include "Core/Graphics/PostProcess.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Engine::Renderers
{
class DX12PostProcess
{
public:
    // samples > 1 renders the scene into a multisampled target (MSAA) that
    // Compose resolves before post-processing; unsupported counts are
    // lowered (SupportedSamples). width x height is the output; a smaller
    // render size (0 = the output) renders the scene into its own target,
    // which TAA upscales (temporal upscaling) or, without TAA, a bilinear
    // pass stretches to the output before bloom, exposure and compose.
    void Create(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t samples = 1u,
        uint32_t renderWidth = 0u, uint32_t renderHeight = 0u);
    uint32_t GetSampleCount() const { return m_samples; }
    bool IsUpscaling() const { return m_sceneColorRender != nullptr; }
    // The highest count up to `samples` that the device supports for the
    // scene colour and depth formats.
    static uint32_t SupportedSamples(ID3D12Device* device, uint32_t samples);
    // Sample count of the scene target bound on a command list by BindScene
    // or RebindScene (1 after Compose). Pipelines pick a matching variant.
    static uint32_t BoundSceneSamples(const ID3D12GraphicsCommandList* commandList);
    void BindScene(ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE depth, const float clearColor[4]);
    void RebindScene(ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE depth);
    void ClearScene(ID3D12GraphicsCommandList* commandList,
        const float clearColor[4]);
    // Bloom passes leave the viewport and scissor at the full scene size,
    // which is what every caller has bound when composing.
    void Compose(ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE output);
    ID3D12Resource* GetSceneColor() const { return m_sceneColor.Get(); }
    // Scene depth (typeless D32_FLOAT_S8X24, resting in DEPTH_WRITE) for
    // temporal anti-aliasing; null leaves TAA off. Set before Compose.
    void SetSceneDepth(ID3D12Resource* depth);

private:
    void RenderBloom(ID3D12GraphicsCommandList* commandList);
    // Meters the scene; returns the compose table that reads this frame's
    // exposure target.
    uint32_t RenderExposure(ID3D12GraphicsCommandList* commandList,
        const Engine::Graphics::PostProcessSettings& settings);
    D3D12_GPU_DESCRIPTOR_HANDLE SrvTable(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SrvSlot(uint32_t index) const;
    // TAA (TemporalResolvePS): blends the scene with its reprojected history
    // into the next history target and copies that into the scene colour,
    // so bloom, exposure and compose read it unchanged.
    void RenderTemporal(ID3D12GraphicsCommandList* commandList,
        const Engine::Graphics::PostProcessSettings& settings);
    // Creates the history targets on first use; (re)writes the temporal
    // tables' history and depth views.
    void PrepareTemporalResources();
    // Stretches the render-size scene into m_sceneColor (UpscalePS).
    void RenderUpscale(ID3D12GraphicsCommandList* commandList);
    // The single-sample target the scene renders into (or MSAA resolves
    // into): m_sceneColorRender when upscaling, else m_sceneColor.
    ID3D12Resource* SceneTarget() const
    { return m_sceneColorRender ? m_sceneColorRender.Get() : m_sceneColor.Get(); }
    static void SetBoundSceneSamples(const ID3D12GraphicsCommandList* commandList, uint32_t samples);

    // Single-sample scene colour that every post pass reads (SRV 0). With
    // MSAA the scene renders into m_sceneColorMultisampled (RTV 0, resting in
    // the resolve-source state) and Compose resolves it into m_sceneColor.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColorMultisampled;
    // Render-size scene colour when the render size is below the output
    // (pixel-shader resource state between frames); null otherwise.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColorRender;
    uint32_t m_samples = 1u;
    // Half-resolution bloom mip chain; every level rests in the pixel-shader
    // resource state between passes.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bloom;
    // Auto exposure (Graphics/PostProcess.h): log-luminance map, histogram
    // and two 1x1 exposure targets alternating frames; all rest in the
    // pixel-shader resource state between passes.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exposureMap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exposureHistogram;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exposure[2];
    Engine::Graphics::ExposureState m_exposureState;
    // RTVs: [0] scene, [1 + level] bloom levels, then exposure map,
    // histogram, exposure 0 and 1 (ExposureRtv).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    // SRVs: [0] scene, [1 + level] bloom levels, two nulls. A table at
    // index i exposes (t0, t1, t2) = entries i..i + 2: the pass reading
    // bloom level k uses 1 + k. Exposure and compose tables follow
    // (ExposureTable).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomDownsamplePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomUpsamplePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_exposureLuminancePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_exposureHistogramPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_exposureAdaptPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_temporalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_upscalePipeline;
    // TAA history, two targets alternating frames (pixel-shader resource
    // state between passes); created on first use.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_history[2];
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    ID3D12Resource* m_sceneDepth = nullptr;
    ID3D12Resource* m_describedDepth = nullptr;
    uint32_t m_historyIndex = 0;
    bool m_historyValid = false;
    uint32_t m_rtvStride = 0;
    uint32_t m_srvStride = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_renderWidth = 0;
    uint32_t m_renderHeight = 0;
    uint32_t m_bloomLevels = 0;
};
}
