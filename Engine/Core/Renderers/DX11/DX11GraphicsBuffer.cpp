#include "pch.h"
#include "DX11GraphicsBuffer.h"
#include <cstring>

namespace Engine::Renderers
{
D3D11GraphicsBuffer::D3D11GraphicsBuffer(
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
    Usage usage, AccessMode access, uint64_t size, const void* initialData,
    uint32_t elementStride, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
    : m_buffer(std::move(buffer)), m_usage(usage), m_access(access), m_size(size),
      m_elementStride(elementStride), m_srv(std::move(srv))
{
    if (access == AccessMode::Upload)
    {
        m_shadowData.resize(static_cast<size_t>(size));
        if (initialData)
            std::memcpy(m_shadowData.data(), initialData, static_cast<size_t>(size));
    }
}

void* D3D11GraphicsBuffer::Map()
{
    if (m_access != AccessMode::Upload)
        throw std::runtime_error("Only upload buffers are CPU-writable in the DX11 backend");
    return m_shadowData.data();
}

void D3D11GraphicsBuffer::Unmap()
{
    if (m_usage == Usage::VertexBuffer)
        FlushMappedWrites();
}

void D3D11GraphicsBuffer::FlushMappedWrites()
{
    if (m_access != AccessMode::Upload || !m_buffer || m_shadowData.empty())
        return;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_buffer->GetDevice(&device);
    if (!device) return;
    device->GetImmediateContext(&context);
    if (!context) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, m_shadowData.data(), m_shadowData.size());
        context->Unmap(m_buffer.Get(), 0);
    }
}

std::unique_ptr<Engine::Graphics::IGraphicsBuffer> D3D11BufferFactory::CreateBuffer(
    Engine::Graphics::IGraphicsBuffer::Usage usage,
    Engine::Graphics::IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes,
    const void* initialData, uint32_t elementStride)
{
    if (!m_device || sizeBytes == 0 || sizeBytes > UINT_MAX)
        throw std::runtime_error("Invalid DX11 buffer request");

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(sizeBytes);
    desc.Usage = access == Engine::Graphics::IGraphicsBuffer::AccessMode::Upload
        ? D3D11_USAGE_DYNAMIC : (access == Engine::Graphics::IGraphicsBuffer::AccessMode::Readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT);
    desc.CPUAccessFlags = access == Engine::Graphics::IGraphicsBuffer::AccessMode::Upload
        ? D3D11_CPU_ACCESS_WRITE : (access == Engine::Graphics::IGraphicsBuffer::AccessMode::Readback ? D3D11_CPU_ACCESS_READ : 0);

    switch (usage)
    {
        case Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer:
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.ByteWidth = (desc.ByteWidth + 15u) & ~15u;
            break;
        case Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer: desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::IndexBuffer: desc.BindFlags = D3D11_BIND_INDEX_BUFFER; break;
        case Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource:
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (!elementStride || sizeBytes % elementStride != 0)
                throw std::runtime_error("Structured DX11 buffers require a valid element stride");
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = elementStride;
            break;
    }
    if (access == Engine::Graphics::IGraphicsBuffer::AccessMode::Readback)
        desc.BindFlags = 0;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initialData;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    ThrowIfFailed(m_device->CreateBuffer(&desc, initialData ? &data : nullptr, &buffer));
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (usage == Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = static_cast<UINT>(sizeBytes / elementStride);
        ThrowIfFailed(m_device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv));
    }
    return std::make_unique<D3D11GraphicsBuffer>(
        std::move(buffer), usage, access, sizeBytes, initialData, elementStride, std::move(srv));
}
}
