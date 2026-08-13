#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "DX11ShaderCompiler.h"
#include "DX11GraphicsBuffer.h"
#include "DX11PipelineStateBuilder.h"
#include "DX11GraphicsContext.h"
#include "DX11GraphicsTexture.h"

namespace Engine::Renderers
{
class D3D11GraphicsProvider : public Engine::Graphics::IGraphicsProvider
{
public:
    D3D11GraphicsProvider(ID3D11Device* device, ID3D11DeviceContext* context);
    Engine::Graphics::IShaderCompiler* GetShaderCompiler() override { return &m_shaderCompiler; }
    Engine::Graphics::IGraphicsBufferFactory* GetBufferFactory() override { return m_bufferFactory.get(); }
    Engine::Graphics::IPipelineStateFactory* GetPipelineStateFactory() override { return m_pipelineFactory.get(); }
    Engine::Graphics::IGraphicsContextFactory* GetContextFactory() override { return m_contextFactory.get(); }
    Engine::Graphics::IGraphicsTextureFactory* GetTextureFactory() override { return m_textureFactory.get(); }

private:
    D3D11ShaderCompiler m_shaderCompiler;
    std::unique_ptr<D3D11BufferFactory> m_bufferFactory;
    std::unique_ptr<D3D11PipelineStateFactory> m_pipelineFactory;
    std::unique_ptr<D3D11GraphicsContextFactory> m_contextFactory;
    std::unique_ptr<D3D11TextureFactory> m_textureFactory;
};
}
