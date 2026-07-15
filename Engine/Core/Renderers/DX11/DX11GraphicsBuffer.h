#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <vector>

class D3D11GraphicsBuffer : public IGraphicsBuffer
{
public:
    D3D11GraphicsBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
                        Usage usage, AccessMode access, uint64_t size,
                        const void* initialData);

    Usage GetUsage() const override { return m_usage; }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t GetSize() const override { return m_size; }
    void* Map() override;
    void Unmap() override {}
    void* GetNativeHandle() const override { return m_buffer.Get(); }

    ID3D11Buffer* GetBuffer() const { return m_buffer.Get(); }
    const uint8_t* GetShadowData() const { return m_shadowData.empty() ? nullptr : m_shadowData.data(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_buffer;
    Usage m_usage;
    AccessMode m_access;
    uint64_t m_size;
    std::vector<uint8_t> m_shadowData;
};

class D3D11BufferFactory : public IGraphicsBufferFactory
{
public:
    explicit D3D11BufferFactory(ID3D11Device* device) : m_device(device) {}

    std::unique_ptr<IGraphicsBuffer> CreateBuffer(
        IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access,
        uint64_t sizeBytes,
        const void* initialData = nullptr) override;

private:
    ID3D11Device* m_device = nullptr;
};
