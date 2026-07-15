#include "pch.h"
#include "DX11GraphicsBuffer.h"
#include <cstring>

D3D11GraphicsBuffer::D3D11GraphicsBuffer(
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
    Usage usage, AccessMode access, uint64_t size, const void* initialData)
    : m_buffer(std::move(buffer)), m_usage(usage), m_access(access), m_size(size)
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

std::unique_ptr<IGraphicsBuffer> D3D11BufferFactory::CreateBuffer(
    IGraphicsBuffer::Usage usage,
    IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes,
    const void* initialData)
{
    if (!m_device || sizeBytes == 0 || sizeBytes > UINT_MAX)
        throw std::runtime_error("Invalid DX11 buffer request");

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(sizeBytes);
    desc.Usage = access == IGraphicsBuffer::AccessMode::Upload
        ? D3D11_USAGE_DYNAMIC : (access == IGraphicsBuffer::AccessMode::Readback ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT);
    desc.CPUAccessFlags = access == IGraphicsBuffer::AccessMode::Upload
        ? D3D11_CPU_ACCESS_WRITE : (access == IGraphicsBuffer::AccessMode::Readback ? D3D11_CPU_ACCESS_READ : 0);

    switch (usage)
    {
        case IGraphicsBuffer::Usage::ConstantBuffer:
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.ByteWidth = (desc.ByteWidth + 15u) & ~15u;
            break;
        case IGraphicsBuffer::Usage::VertexBuffer: desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; break;
        case IGraphicsBuffer::Usage::IndexBuffer: desc.BindFlags = D3D11_BIND_INDEX_BUFFER; break;
        case IGraphicsBuffer::Usage::ShaderResource: desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; break;
    }
    if (access == IGraphicsBuffer::AccessMode::Readback)
        desc.BindFlags = 0;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initialData;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    ThrowIfFailed(m_device->CreateBuffer(&desc, initialData ? &data : nullptr, &buffer));
    return std::make_unique<D3D11GraphicsBuffer>(
        std::move(buffer), usage, access, sizeBytes, initialData);
}
