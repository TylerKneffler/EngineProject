#include "pch.h"
#include "DX11GraphicsProvider.h"

D3D11GraphicsProvider::D3D11GraphicsProvider(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_bufferFactory = std::make_unique<D3D11BufferFactory>(device);
    m_pipelineFactory = std::make_unique<D3D11PipelineStateFactory>(device);
    m_contextFactory = std::make_unique<D3D11GraphicsContextFactory>(device, context);
}
