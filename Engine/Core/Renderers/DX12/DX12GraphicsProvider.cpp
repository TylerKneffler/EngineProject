#include "DX12GraphicsProvider.h"

D3D12GraphicsProvider::D3D12GraphicsProvider(
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    ID3D12RootSignature* rootSig)
{
    OutputDebugStringA("[D3D12GraphicsProvider] Creating shader compiler...\n");
    m_shaderCompiler = std::make_unique<D3D12ShaderCompiler>();
    OutputDebugStringA("[D3D12GraphicsProvider] Creating buffer factory...\n");
    m_bufferFactory = std::make_unique<D3D12BufferFactory>(device);
    OutputDebugStringA("[D3D12GraphicsProvider] Creating pipeline factory...\n");
    m_pipelineFactory = std::make_unique<D3D12PipelineStateFactory>(device, rootSig);
    OutputDebugStringA("[D3D12GraphicsProvider] Creating context factory...\n");
    m_contextFactory = std::make_unique<D3D12GraphicsContextFactory>(device, commandQueue, rootSig);
    OutputDebugStringA("[D3D12GraphicsProvider] All factories created\n");
}
