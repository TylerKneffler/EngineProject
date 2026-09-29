#include "pch.h"
#include "DX11GraphicsBuffer.h"
#include <algorithm>
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
    // Map() exposes the CPU shadow for every upload-buffer usage.  Callers
    // reasonably expect Unmap() to publish those writes regardless of whether
    // the buffer contains vertices, indices, constants, or structured data.
    // Restricting this to vertex buffers left reused terrain index buffers on
    // the GPU with their previous chunk's topology: new vertices were then
    // connected by stale indices, producing long stretched triangles along
    // streamed chunk edges.
    FlushMappedWrites();
}

void D3D11GraphicsBuffer::FlushMappedWrites(uint64_t offset, uint64_t size)
{
    if (m_access != AccessMode::Upload || !m_buffer || m_shadowData.empty() ||
        offset >= m_size || size == 0)
        return;
    // D3D11_MAP_WRITE_DISCARD invalidates the complete native resource, not
    // merely the byte range requested by the caller.  The range still guards
    // empty/out-of-bounds flushes above, but a successful discard must restore
    // the entire CPU shadow.  Copying only [offset, offset + size) caused later
    // partial updates (morph weights, portal records, and editor debug tails)
    // to leave every untouched byte in the GPU buffer undefined.
    const size_t byteCount = static_cast<size_t>(m_size);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_buffer->GetDevice(&device);
    if (!device) return;
    device->GetImmediateContext(&context);
    if (!context) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(m_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, m_shadowData.data(), byteCount);
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
            // D3D11GraphicsContext snapshots one logical record from the CPU
            // shadow arena into its own native constant buffer at draw time.
            // Keep this placeholder legal when the logical arena contains
            // stable ranges for many deferred portal draws.
            desc.ByteWidth = std::min<UINT>(
                (desc.ByteWidth + 15u) & ~15u,
                D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u);
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
