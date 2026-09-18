#pragma once
#include <cstdint>
#include <memory>

namespace Engine::Graphics
{

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
    virtual uint32_t GetElementStride() const { return 0; }
    // Extra system-memory copy retained by a backend for deferred uploads.
    // This is separate from both the owning mesh and the native GPU buffer.
    virtual uint64_t GetUploadShadowSize() const { return 0; }

    // Map for CPU access (for Upload buffers)
    // Returns nullptr if buffer is not mappable
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
    // Makes a written byte range visible when a backend uses a CPU shadow
    // allocation (currently D3D11). Other backends are coherent. UINT64_MAX
    // means the remainder of the buffer.
    virtual void FlushMappedWrites(uint64_t = 0,
        uint64_t = UINT64_MAX) {}

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
        const void* initialData = nullptr,
        uint32_t elementStride = 0) = 0;
};
}
