#include "DX12GraphicsProvider.h"

D3D12GraphicsProvider::D3D12GraphicsProvider(
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    ID3D12RootSignature* rootSig)
{
    m_shaderCompiler = std::make_unique<D3D12ShaderCompiler>();
    m_bufferFactory = std::make_unique<D3D12BufferFactory>(device);
    m_pipelineFactory = std::make_unique<D3D12PipelineStateFactory>(device, rootSig);
    m_contextFactory = std::make_unique<D3D12GraphicsContextFactory>(device, commandQueue);
}
