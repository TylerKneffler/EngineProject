#pragma once

#include "Core/Graphics/PostProcess.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace Engine::Renderers
{
class DX12PostProcess
{
public:
    void Create(ID3D12Device* device, uint32_t width, uint32_t height);
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

private:
    void RenderBloom(ID3D12GraphicsCommandList* commandList);
    // Meters the scene; returns the compose table that reads this frame's
    // exposure target.
    uint32_t RenderExposure(ID3D12GraphicsCommandList* commandList,
        const Engine::Graphics::PostProcessSettings& settings);
    D3D12_GPU_DESCRIPTOR_HANDLE SrvTable(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor;
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
    uint32_t m_rtvStride = 0;
    uint32_t m_srvStride = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_bloomLevels = 0;
};
}
