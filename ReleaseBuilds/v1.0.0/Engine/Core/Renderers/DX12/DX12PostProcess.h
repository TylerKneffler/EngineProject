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
    void ClearScene(ID3D12GraphicsCommandList* commandList,
        const float clearColor[4]);
    void Compose(ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE output);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
}
