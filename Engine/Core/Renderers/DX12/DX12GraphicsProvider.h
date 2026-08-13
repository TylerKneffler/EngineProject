#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "DX12ShaderCompiler.h"
#include "DX12GraphicsBuffer.h"
#include "DX12PipelineStateBuilder.h"
#include "DX12GraphicsContext.h"
#include "DX12GraphicsTexture.h"
#include <memory>
#include <d3d12.h>

namespace Engine::Renderers
{
// ---------------------------------------------------------------------------
// D3D12GraphicsProvider — Provides graphics services for D3D12 rendering
//
// This is owned by D3D12EditorRenderer and D3D12GameRenderer and provides
// access to all graphics factories needed by Scene, Mesh, View, etc.
// ---------------------------------------------------------------------------
class D3D12GraphicsProvider : public Engine::Graphics::IGraphicsProvider
{
public:
    D3D12GraphicsProvider(
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        ID3D12RootSignature* rootSig);

    Engine::Graphics::IShaderCompiler* GetShaderCompiler() override { return m_shaderCompiler.get(); }
    Engine::Graphics::IGraphicsBufferFactory* GetBufferFactory() override { return m_bufferFactory.get(); }
    Engine::Graphics::IPipelineStateFactory* GetPipelineStateFactory() override { return m_pipelineFactory.get(); }
    Engine::Graphics::IGraphicsContextFactory* GetContextFactory() override { return m_contextFactory.get(); }
    Engine::Graphics::IGraphicsTextureFactory* GetTextureFactory() override { return m_textureFactory.get(); }

private:
    std::unique_ptr<D3D12ShaderCompiler> m_shaderCompiler;
    std::unique_ptr<D3D12BufferFactory> m_bufferFactory;
    std::unique_ptr<D3D12PipelineStateFactory> m_pipelineFactory;
    std::unique_ptr<D3D12GraphicsContextFactory> m_contextFactory;
    std::unique_ptr<D3D12TextureFactory> m_textureFactory;
};

Microsoft::WRL::ComPtr<ID3D12RootSignature>
CreateD3D12MaterialRootSignature(ID3D12Device* device);
}
