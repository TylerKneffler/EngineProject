#include "DX12GraphicsBuffer.h"
#include "DX12QueueMutex.h"
#include "DX12DredDiagnostics.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <atomic>
#include <mutex>

namespace Engine::Renderers
{
namespace
{
void LogD3D12BufferAddress(ID3D12Resource* resource,
    Engine::Graphics::IGraphicsBuffer::Usage usage,
    Engine::Graphics::IGraphicsBuffer::AccessMode access,
    uint64_t sizeBytes)
{
#if defined(_DEBUG)
    static std::atomic<uint64_t> nextId{1};
    static std::mutex logMutex;
    const uint64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream name;
    name << "EngineBuffer#" << id << " bytes=" << sizeBytes
        << " usage=" << static_cast<int>(usage)
        << " access=" << static_cast<int>(access);
    const std::string label = name.str();
    std::wstring wideLabel(label.begin(), label.end());
    resource->SetName(wideLabel.c_str());
    std::lock_guard lock(logMutex);
    std::ofstream log("dx12-buffer-addresses.log", std::ios::app);
    if (log)
        log << "id=" << id << " gpuVA=0x" << std::hex
            << resource->GetGPUVirtualAddress() << std::dec
            << " bytes=" << sizeBytes
            << " usage=" << static_cast<int>(usage)
            << " access=" << static_cast<int>(access) << '\n';
#else
    (void)resource;
    (void)usage;
    (void)access;
    (void)sizeBytes;
#endif
}
}

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

    // D3D12 buffers are created in COMMON regardless of the requested
    // InitialState. Keep the tracker in COMMON as well; the first binding
    // then emits an explicit transition to vertex/index/SRV/UAV state.
    if (access == Engine::Graphics::IGraphicsBuffer::AccessMode::Default)
        initialState = D3D12_RESOURCE_STATE_COMMON;

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
        AppendDredDiagnostics(m_device, message);
        OutputDebugStringA((message.str() + "\n").c_str());
        // Preserve allocation diagnostics even when the editor exits without
        // an attached debugger or a visible console.
        std::ofstream log("dx12-buffer-error.log", std::ios::app);
        if (log) log << message.str() << std::endl;
        throw std::runtime_error(message.str());
    }

    auto buffer = std::make_unique<D3D12GraphicsBuffer>(resource, usage, access, sizeBytes);
    LogD3D12BufferAddress(resource.Get(), usage, access, sizeBytes);
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
    std::unique_ptr<Buffer> destination, staging;
    constexpr uint64_t stagingCacheLimit = 16ull * 1024 * 1024;
    {
        Engine::Serialization::PrefabLoadProfile::Stage stage("dx12_static_allocate_copy");
        {
        Engine::Serialization::PrefabLoadProfile::Stage allocation("dx12_static_destination_allocate");
        destination = CreateBuffer(Buffer::Usage::VertexBuffer,
            Buffer::AccessMode::Default, sizeBytes);
        }
        {
        Engine::Serialization::PrefabLoadProfile::Stage allocation("dx12_static_staging_allocate");
        if (sizeBytes <= stagingCacheLimit)
        {
            std::lock_guard lock(m_staticStagingMutex);
            staging = std::move(m_staticStaging);
        }
        if (!staging || staging->GetSize() < sizeBytes)
        {
            Engine::Serialization::PrefabLoadProfile::Stage fresh("dx12_static_staging_new");
            // Grow geometrically within the cap to avoid reallocating for each
            // slightly larger part. Oversized uploads are never retained.
            const uint64_t capacity = sizeBytes > stagingCacheLimit ? sizeBytes :
                std::max(sizeBytes, std::min(stagingCacheLimit,
                    staging ? staging->GetSize() * 2 : uint64_t(64 * 1024)));
            staging = CreateBuffer(Buffer::Usage::VertexBuffer,
                Buffer::AccessMode::Upload, capacity);
        }
        }
        {
        Engine::Serialization::PrefabLoadProfile::Stage copy("dx12_static_staging_copy");
        void* mapped = staging->Map();
        std::memcpy(mapped, initialData, sizeBytes);
        staging->Unmap();
        }
    }
    auto* native = static_cast<D3D12GraphicsBuffer*>(destination.get());
    auto* upload = static_cast<D3D12GraphicsBuffer*>(staging.get());
    std::unique_ptr<StaticUploadCommands> bundle;
    {
    Engine::Serialization::PrefabLoadProfile::Stage stage("dx12_static_record");
    {
        std::lock_guard lock(m_staticStagingMutex);
        bundle = std::move(m_staticCommands);
    }
    if (bundle)
    {
        Engine::Serialization::PrefabLoadProfile::Stage reuse("dx12_static_commands_reuse");
        // A bundle can enter the cache only after its own fence completed.
        if (FAILED(bundle->allocator->Reset()) ||
            FAILED(bundle->commands->Reset(bundle->allocator.Get(), nullptr)))
            throw std::runtime_error("Failed to reset static geometry upload commands");
    }
    else
    {
        Engine::Serialization::PrefabLoadProfile::Stage fresh("dx12_static_commands_new");
        bundle = std::make_unique<StaticUploadCommands>();
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&bundle->allocator))) ||
            FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                bundle->allocator.Get(), nullptr, IID_PPV_ARGS(&bundle->commands))) ||
            FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&bundle->fence))))
            throw std::runtime_error("Failed to create static geometry upload commands");
        bundle->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!bundle->event)
            throw std::runtime_error("Failed to create static geometry upload event");
    }
    auto* commands = bundle->commands.Get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = native->GetResource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyBufferRegion(native->GetResource(), 0, upload->GetResource(), 0, sizeBytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close()))
        throw std::runtime_error("Failed to close static geometry upload commands");
    }
    if (bundle->fenceValue == UINT64_MAX)
        throw std::runtime_error("Static geometry upload fence exhausted");
    const uint64_t completion = ++bundle->fenceValue;
    if (FAILED(bundle->fence->SetEventOnCompletion(completion, bundle->event)))
        throw std::runtime_error("Failed to arm static geometry upload fence");
    {
        Engine::Serialization::PrefabLoadProfile::Stage stage("dx12_static_submit");
        // Use the same submission lock as rendering and texture uploads. Wait
        // outside the lock, and retain staging/commands until the copy finishes.
        std::lock_guard queueLock(DX12QueueMutex());
        ID3D12CommandList* lists[] = {bundle->commands.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        if (FAILED(m_queue->Signal(bundle->fence.Get(), completion)))
            throw std::runtime_error("Failed to signal static geometry upload fence");
    }
    {
        Engine::Serialization::PrefabLoadProfile::Stage stage("dx12_static_wait");
        if (WaitForSingleObject(bundle->event, INFINITE) != WAIT_OBJECT_0 ||
            bundle->fence->GetCompletedValue() == UINT64_MAX ||
            bundle->fence->GetCompletedValue() < completion)
            throw std::runtime_error("Failed to wait for static geometry upload");
    }
    native->SetResourceState(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    {
        std::lock_guard lock(m_staticStagingMutex);
        if (!m_staticCommands) m_staticCommands = std::move(bundle);
    }
    if (staging->GetSize() <= stagingCacheLimit)
    {
        std::lock_guard lock(m_staticStagingMutex);
        // Concurrent uploads may return another buffer first. Keep the larger
        // scratch allocation and release the other after leaving this lock.
        if (!m_staticStaging || m_staticStaging->GetSize() < staging->GetSize())
            m_staticStaging.swap(staging);
    }
    return destination;
}

}
