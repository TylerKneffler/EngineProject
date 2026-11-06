#include "DX12GraphicsBuffer.h"
#include "DX12QueueMutex.h"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <fstream>

namespace Engine::Renderers
{

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

    D3D12_RANGE readRange = m_access == AccessMode::Readback
        ? D3D12_RANGE{ 0, static_cast<SIZE_T>(m_size) }
        : D3D12_RANGE{ 0, 0 };
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

std::unique_ptr<Engine::Graphics::IGraphicsBuffer> D3D12BufferFactory::CreateBuffer(
    Engine::Graphics::IGraphicsBuffer::Usage usage,
    Engine::Graphics::IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes,
    const void* initialData, uint32_t elementStride)
{
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

    // Determine heap type based on access mode
    switch (access)
    {
        case Engine::Graphics::IGraphicsBuffer::AccessMode::Default:
            heapType = D3D12_HEAP_TYPE_DEFAULT;
            break;
        case Engine::Graphics::IGraphicsBuffer::AccessMode::Upload:
            heapType = D3D12_HEAP_TYPE_UPLOAD;
            initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case Engine::Graphics::IGraphicsBuffer::AccessMode::Readback:
            heapType = D3D12_HEAP_TYPE_READBACK;
            initialState = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
    }

    // Set initial state based on usage
    if (access == Engine::Graphics::IGraphicsBuffer::AccessMode::Default)
    {
        switch (usage)
        {
            case Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer:
            case Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer:
                initialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                break;
            case Engine::Graphics::IGraphicsBuffer::Usage::IndexBuffer:
                initialState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
                break;
            case Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource:
                initialState = D3D12_RESOURCE_STATE_COMMON;
                break;
            case Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer:
                initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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
    if (usage == Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer &&
        access == Engine::Graphics::IGraphicsBuffer::AccessMode::Default)
        bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
    {
        std::ostringstream message;
        message << "Failed to create D3D12 buffer: HRESULT=0x"
            << std::hex << static_cast<unsigned long>(hr) << std::dec
            << ", bytes=" << sizeBytes
            << ", usage=" << static_cast<int>(usage)
            << ", access=" << static_cast<int>(access)
            << ", stride=" << elementStride;
        const HRESULT removedReason = m_device->GetDeviceRemovedReason();
        if (FAILED(removedReason))
            message << ", deviceRemovedReason=0x" << std::hex
                << static_cast<unsigned long>(removedReason);
        OutputDebugStringA((message.str() + "\n").c_str());
        // Preserve allocation diagnostics even when the editor exits without
        // an attached debugger or a visible console.
        std::ofstream log("dx12-buffer-error.log", std::ios::app);
        if (log) log << message.str() << std::endl;
        throw std::runtime_error(message.str());
    }

    auto buffer = std::make_unique<D3D12GraphicsBuffer>(resource, usage, access, sizeBytes);
    buffer->SetResourceState(initialState);
    buffer->SetElementStride(elementStride);

    // If initial data provided, copy it
    if (initialData && access == Engine::Graphics::IGraphicsBuffer::AccessMode::Upload)
    {
        void* mapped = buffer->Map();
        memcpy(mapped, initialData, sizeBytes);
        buffer->Unmap();
    }

    return buffer;
}

std::unique_ptr<Engine::Graphics::IGraphicsBuffer> D3D12BufferFactory::CreateStaticVertexBuffer(
    uint64_t sizeBytes, const void* initialData)
{
    using Buffer = Engine::Graphics::IGraphicsBuffer;
    if (!m_queue || !initialData || !sizeBytes)
        return IGraphicsBufferFactory::CreateStaticVertexBuffer(sizeBytes, initialData);

    // Static geometry is read many times per frame by forest instances. Keep
    // it in a default heap rather than repeatedly fetching upload-heap data.
    auto destination = CreateBuffer(Buffer::Usage::VertexBuffer,
        Buffer::AccessMode::Default, sizeBytes);
    auto staging = CreateBuffer(Buffer::Usage::VertexBuffer,
        Buffer::AccessMode::Upload, sizeBytes, initialData);
    auto* native = static_cast<D3D12GraphicsBuffer*>(destination.get());
    auto* upload = static_cast<D3D12GraphicsBuffer*>(staging.get());
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(&commands))) ||
        FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        throw std::runtime_error("Failed to create static geometry upload commands");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = native->GetResource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(native->GetResource(), 0, upload->GetResource(), 0, sizeBytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close()))
        throw std::runtime_error("Failed to close static geometry upload commands");
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle)
        throw std::runtime_error("Failed to create static geometry upload event");
    struct CloseEvent { HANDLE handle; ~CloseEvent() { CloseHandle(handle); } } closeEvent{eventHandle};
    if (FAILED(fence->SetEventOnCompletion(1, eventHandle)))
        throw std::runtime_error("Failed to arm static geometry upload fence");
    {
        // Use the same submission lock as rendering and texture uploads. Wait
        // outside the lock, and retain staging/commands until the copy finishes.
        std::lock_guard queueLock(DX12QueueMutex());
        ID3D12CommandList* lists[] = {commands.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        if (FAILED(m_queue->Signal(fence.Get(), 1)))
            throw std::runtime_error("Failed to signal static geometry upload fence");
    }
    if (WaitForSingleObject(eventHandle, INFINITE) != WAIT_OBJECT_0)
        throw std::runtime_error("Failed to wait for static geometry upload");
    return destination;
}

}
