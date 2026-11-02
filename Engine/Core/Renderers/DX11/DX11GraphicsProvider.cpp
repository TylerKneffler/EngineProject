#include "pch.h"
#include "DX11GraphicsProvider.h"

namespace Engine::Renderers
{
D3D11GraphicsProvider::D3D11GraphicsProvider(ID3D11Device* device, ID3D11DeviceContext* context)
{
    auto native=std::make_shared<Engine::Graphics::NativeSubmissionProbe>();
    native->timings.supported=true;
    m_bufferFactory = std::make_unique<D3D11BufferFactory>(device,native);
    m_pipelineFactory = std::make_unique<D3D11PipelineStateFactory>(device);
    m_contextFactory = std::make_unique<D3D11GraphicsContextFactory>(device, context,native);
    m_textureFactory = std::make_unique<D3D11TextureFactory>(device);
}
}
