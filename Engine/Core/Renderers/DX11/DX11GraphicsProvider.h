#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "DX11ShaderCompiler.h"
#include "DX11GraphicsBuffer.h"
#include "DX11PipelineStateBuilder.h"
#include "DX11GraphicsContext.h"

class D3D11GraphicsProvider : public IGraphicsProvider
{
public:
    D3D11GraphicsProvider(ID3D11Device* device, ID3D11DeviceContext* context);
    IShaderCompiler* GetShaderCompiler() override { return &m_shaderCompiler; }
    IGraphicsBufferFactory* GetBufferFactory() override { return m_bufferFactory.get(); }
    IPipelineStateFactory* GetPipelineStateFactory() override { return m_pipelineFactory.get(); }
    IGraphicsContextFactory* GetContextFactory() override { return m_contextFactory.get(); }

private:
    D3D11ShaderCompiler m_shaderCompiler;
    std::unique_ptr<D3D11BufferFactory> m_bufferFactory;
    std::unique_ptr<D3D11PipelineStateFactory> m_pipelineFactory;
    std::unique_ptr<D3D11GraphicsContextFactory> m_contextFactory;
};
