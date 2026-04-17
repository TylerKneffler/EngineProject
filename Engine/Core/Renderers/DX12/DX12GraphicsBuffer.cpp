#include "DX12GraphicsBuffer.h"
#include <stdexcept>

D3D12GraphicsBuffer::D3D12GraphicsBuffer(
    Microsoft::WRL::ComPtr<ID3D12Resource> resource,
    Usage usage,
    AccessMode access,
    uint64_t size)
    : m_resource(resource), m_usage(usage), m_access(access), m_size(size)
{
}

D3D12GraphicsBuffer::~D3D12GraphicsBuffer()
{
    if (m_mappedData)
        Unmap();
}

void* D3D12GraphicsBuffer::Map()
{
    if (m_mappedData)
        return m_mappedData;  // Already mapped

    if (m_access == AccessMode::Default)
        throw std::runtime_error("Cannot map a Default access buffer");

    D3D12_RANGE readRange = { 0, 0 };  // We're writing, no read range needed
    HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr))
        throw std::runtime_error("Failed to map D3D12 buffer");

    return m_mappedData;
}

void D3D12GraphicsBuffer::Unmap()
{
    if (m_mappedData)
    {
        m_resource->Unmap(0, nullptr);
        m_mappedData = nullptr;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12GraphicsBuffer::GetGPUVirtualAddress() const
{
    return m_resource->GetGPUVirtualAddress();
}

// ---------------------------------------------------------------------------
// D3D12BufferFactory::CreateBuffer
// ---------------------------------------------------------------------------

std::unique_ptr<IGraphicsBuffer> D3D12BufferFactory::CreateBuffer(
    IGraphicsBuffer::Usage usage,
    IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes,
    const void* initialData)
{
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

    // Determine heap type based on access mode
    switch (access)
    {
        case IGraphicsBuffer::AccessMode::Default:
            heapType = D3D12_HEAP_TYPE_DEFAULT;
            break;
        case IGraphicsBuffer::AccessMode::Upload:
            heapType = D3D12_HEAP_TYPE_UPLOAD;
            initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case IGraphicsBuffer::AccessMode::Readback:
            heapType = D3D12_HEAP_TYPE_READBACK;
            initialState = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
    }

    // Set initial state based on usage
    if (access == IGraphicsBuffer::AccessMode::Default)
    {
        switch (usage)
        {
            case IGraphicsBuffer::Usage::ConstantBuffer:
            case IGraphicsBuffer::Usage::VertexBuffer:
                initialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                break;
            case IGraphicsBuffer::Usage::IndexBuffer:
                initialState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
                break;
            case IGraphicsBuffer::Usage::ShaderResource:
                initialState = D3D12_RESOURCE_STATE_COMMON;
                break;
        }
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = sizeBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
        throw std::runtime_error("Failed to create D3D12 buffer");

    auto buffer = std::make_unique<D3D12GraphicsBuffer>(resource, usage, access, sizeBytes);

    // If initial data provided, copy it
    if (initialData && access == IGraphicsBuffer::AccessMode::Upload)
    {
        void* mapped = buffer->Map();
        memcpy(mapped, initialData, sizeBytes);
        buffer->Unmap();
    }

    return buffer;
}
