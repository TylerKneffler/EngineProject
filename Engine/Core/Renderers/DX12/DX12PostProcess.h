#pragma once

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
    D3D12_GPU_DESCRIPTOR_HANDLE SrvTable(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t index) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor;
    // Half-resolution bloom mip chain; every level rests in the pixel-shader
    // resource state between passes.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bloom;
    // RTVs: [0] scene, [1 + level] bloom levels.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    // SRVs: [0] scene, [1 + level] bloom levels, [last] null. A table at
    // index i exposes (t0, t1) = (entry i, entry i + 1): compose uses 0
    // (scene, bloom level 0) and the pass reading level k uses 1 + k.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomDownsamplePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bloomUpsamplePipeline;
    uint32_t m_rtvStride = 0;
    uint32_t m_srvStride = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_bloomLevels = 0;
};
}
