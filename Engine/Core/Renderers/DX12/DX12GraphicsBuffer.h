#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"
#include <wrl/client.h>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// D3D12GraphicsBuffer — DirectX 12 GPU buffer wrapper
// ---------------------------------------------------------------------------
class D3D12GraphicsBuffer : public IGraphicsBuffer
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
};

// ---------------------------------------------------------------------------
// D3D12BufferFactory — Create DirectX 12 buffers
// ---------------------------------------------------------------------------
class D3D12BufferFactory : public IGraphicsBufferFactory
{
public:
    explicit D3D12BufferFactory(ID3D12Device* device)
        : m_device(device) {}

    std::unique_ptr<IGraphicsBuffer> CreateBuffer(
        IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access,
        uint64_t sizeBytes,
        const void* initialData = nullptr) override;

private:
    ID3D12Device* m_device;
};
