#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "DX12ShaderCompiler.h"
#include "DX12GraphicsBuffer.h"
#include "DX12PipelineStateBuilder.h"
#include "DX12GraphicsContext.h"
#include <memory>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// D3D12GraphicsProvider — Provides graphics services for D3D12 rendering
//
// This is owned by D3D12EditorRenderer and D3D12GameRenderer and provides
// access to all graphics factories needed by Scene, Mesh, View, etc.
// ---------------------------------------------------------------------------
class D3D12GraphicsProvider : public IGraphicsProvider
{
public:
    D3D12GraphicsProvider(
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        ID3D12RootSignature* rootSig);

    IShaderCompiler* GetShaderCompiler() override { return m_shaderCompiler.get(); }
    IGraphicsBufferFactory* GetBufferFactory() override { return m_bufferFactory.get(); }
    IPipelineStateFactory* GetPipelineStateFactory() override { return m_pipelineFactory.get(); }
    IGraphicsContextFactory* GetContextFactory() override { return m_contextFactory.get(); }

private:
    std::unique_ptr<D3D12ShaderCompiler> m_shaderCompiler;
    std::unique_ptr<D3D12BufferFactory> m_bufferFactory;
    std::unique_ptr<D3D12PipelineStateFactory> m_pipelineFactory;
    std::unique_ptr<D3D12GraphicsContextFactory> m_contextFactory;
};
