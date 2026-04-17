#pragma once
#include <cstdint>
#include <memory>

// ---------------------------------------------------------------------------
// IGraphicsBuffer — GPU-side buffer (constant, vertex, index, etc.)
// 
// Abstracts ID3D12Resource and Vulkan VkBuffer
// ---------------------------------------------------------------------------
class IGraphicsBuffer
{
public:
    virtual ~IGraphicsBuffer() = default;

    enum class Usage
    {
        ConstantBuffer,
        VertexBuffer,
        IndexBuffer,
        ShaderResource,
    };

    enum class AccessMode
    {
        Default,    // GPU read/write only
        Upload,     // CPU write → GPU read
        Readback,   // GPU write → CPU read
    };

    virtual Usage GetUsage() const = 0;
    virtual AccessMode GetAccessMode() const = 0;
    virtual uint64_t GetSize() const = 0;

    // Map for CPU access (for Upload buffers)
    // Returns nullptr if buffer is not mappable
    virtual void* Map() = 0;
    virtual void Unmap() = 0;

    // For internal use: get D3D12 GPU virtual address, Vulkan buffer, etc.
    // The actual type depends on the graphics API
    virtual void* GetNativeHandle() const = 0;
};

// ---------------------------------------------------------------------------
// IGraphicsBufferFactory — Creates graphics buffers
// 
// Each renderer implements this to create D3D12, Vulkan, Metal, etc. buffers
// ---------------------------------------------------------------------------
class IGraphicsBufferFactory
{
public:
    virtual ~IGraphicsBufferFactory() = default;

    virtual std::unique_ptr<IGraphicsBuffer> CreateBuffer(
        IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access,
        uint64_t sizeBytes,
        const void* initialData = nullptr) = 0;
};
