#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <mutex>

// ---------------------------------------------------------------------------
// D3D12GraphicsBuffer — DirectX 12 GPU buffer wrapper
// ---------------------------------------------------------------------------

namespace Engine::Renderers
{
class D3D12GraphicsBuffer : public Engine::Graphics::IGraphicsBuffer
{
public:
    D3D12GraphicsBuffer(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        Usage usage,
        AccessMode access,
        uint64_t size);

    ~D3D12GraphicsBuffer() override;

    Usage GetUsage() const override { return m_usage; }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t GetSize() const override { return m_size; }
    uint32_t GetElementStride() const override { return m_elementStride; }
    D3D12_RESOURCE_STATES GetResourceState() const { return m_resourceState; }
    void SetResourceState(D3D12_RESOURCE_STATES state) { m_resourceState = state; }
    void SetElementStride(uint32_t value) { m_elementStride = value; }

    void* Map() override;
    void Unmap() override;

    void* GetNativeHandle() const override { return m_resource.Get(); }
    
    // D3D12-specific accessors
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Usage m_usage;
    AccessMode m_access;
    uint64_t m_size;
    void* m_mappedData = nullptr;
    uint32_t m_elementStride = 0;
    D3D12_RESOURCE_STATES m_resourceState = D3D12_RESOURCE_STATE_COMMON;
};

// ---------------------------------------------------------------------------
// D3D12BufferFactory — Create DirectX 12 buffers
// ---------------------------------------------------------------------------
class D3D12BufferFactory : public Engine::Graphics::IGraphicsBufferFactory
{
public:
    explicit D3D12BufferFactory(ID3D12Device* device, ID3D12CommandQueue* queue = nullptr)
        : m_device(device), m_queue(queue) {}

    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateBuffer(
        Engine::Graphics::IGraphicsBuffer::Usage usage,
        Engine::Graphics::IGraphicsBuffer::AccessMode access,
        uint64_t sizeBytes,
        const void* initialData = nullptr,
        uint32_t elementStride = 0) override;

    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateStaticVertexBuffer(
        uint64_t sizeBytes, const void* initialData) override;

private:
    ID3D12Device* m_device;
    ID3D12CommandQueue* m_queue;
    // One bounded upload scratch buffer, returned only after its copy finishes.
    std::mutex m_staticStagingMutex;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> m_staticStaging;
    struct StaticUploadCommands
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        uint64_t fenceValue = 0;
        ~StaticUploadCommands() { if (event) CloseHandle(event); }
    };
    // One idle bundle. Each upload leases it exclusively and returns it only
    // after completion. Concurrent uploads use independent local bundles.
    std::unique_ptr<StaticUploadCommands> m_staticCommands;
};
}
