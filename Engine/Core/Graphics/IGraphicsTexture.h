#pragma once

#include <cstdint>
#include <memory>
#include <algorithm>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <atomic>

namespace Engine::Graphics
{

enum class GraphicsTextureFormat
{
    Rgba8,
    Rgba32Float,
    // 4x4 block-compressed. Mip chains are tightly packed, one level after
    // another, each level stored as rows of blocks.
    Bc1, // RGB + 1-bit alpha, 8 bytes per block
    Bc4, // one channel (height, roughness), 8 bytes per block
    Bc5, // two channels (tangent-space normal XY), 16 bytes per block
    Bc7  // RGBA, 16 bytes per block
};

inline bool GraphicsTextureIsBlockCompressed(GraphicsTextureFormat format)
{
    return format == GraphicsTextureFormat::Bc1 || format == GraphicsTextureFormat::Bc4 ||
        format == GraphicsTextureFormat::Bc5 || format == GraphicsTextureFormat::Bc7;
}

// Uncompressed formats only; block formats have no per-pixel size.
inline uint32_t GraphicsTextureBytesPerPixel(GraphicsTextureFormat format)
{
    return format == GraphicsTextureFormat::Rgba32Float ? 16u : 4u;
}

inline uint32_t GraphicsTextureBytesPerBlock(GraphicsTextureFormat format)
{
    return format == GraphicsTextureFormat::Bc1 || format == GraphicsTextureFormat::Bc4 ? 8u : 16u;
}

// Bytes in one stored row: a row of pixels, or a row of 4x4 blocks.
inline uint32_t GraphicsTextureRowBytes(GraphicsTextureFormat format, uint32_t width)
{
    return GraphicsTextureIsBlockCompressed(format)
        ? ((width + 3u) / 4u) * GraphicsTextureBytesPerBlock(format)
        : width * GraphicsTextureBytesPerPixel(format);
}

inline uint32_t GraphicsTextureRowCount(GraphicsTextureFormat format, uint32_t height)
{
    return GraphicsTextureIsBlockCompressed(format) ? (height + 3u) / 4u : height;
}

inline uint64_t GraphicsTextureMipBytes(GraphicsTextureFormat format, uint32_t width, uint32_t height)
{
    return uint64_t(GraphicsTextureRowBytes(format, width)) * GraphicsTextureRowCount(format, height);
}

// D3D rejects block-compressed textures whose top level is not a whole number
// of blocks, so only such levels can start a streamed mip tail.
inline bool GraphicsTextureIsValidBase(GraphicsTextureFormat format, uint32_t width, uint32_t height)
{
    return !GraphicsTextureIsBlockCompressed(format) || (width % 4u == 0 && height % 4u == 0);
}

inline uint64_t GraphicsTextureChainBytes(GraphicsTextureFormat format, uint32_t width,
    uint32_t height, uint32_t levels)
{
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < levels && width && height; ++mip)
    {
        bytes += GraphicsTextureMipBytes(format, width, height);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return bytes;
}

class IGraphicsTexture : public std::enable_shared_from_this<IGraphicsTexture>
{
public:
    virtual ~IGraphicsTexture() = default;
    virtual void* GetNativeHandle() const = 0;
    virtual bool IsUploadComplete() const { return true; }
    virtual uint64_t GetAllocationSizeBytes() const { return 0; }
    // Handle suitable for editor UI image rendering (e.g. ImGui texture ID).
    // Defaults to the backend native handle; renderers can override when UI
    // requires a different object (such as DX12 GPU descriptor handles).
    virtual void* GetUiHandle() const { return GetNativeHandle(); }
};

// Slots are reset only after the host renderer has completed their GPU fence.
// Covers direct and worker command recording, independently of scene packets.
class TextureFrameOwners
{
public:
    void Prepare(uint32_t slot)
    {
        std::lock_guard lock(m_mutex);
        if (m_slots.size() <= slot) m_slots.resize(size_t(slot) + 1);
        m_slot = slot;
        m_slots[slot].owners.clear();
        m_slots[slot].seen.clear();
        ++m_generation;
    }
    void Retain(const IGraphicsTexture* texture)
    {
        if (!texture) return;
        std::lock_guard lock(m_mutex);
        if (m_slots.empty()) m_slots.resize(1);
        auto& slot = m_slots[m_slot];
        if (slot.seen.insert(texture).second)
            if (auto owner = texture->weak_from_this().lock()) slot.owners.push_back(std::move(owner));
    }
    uint64_t Generation() const { return m_generation.load(std::memory_order_relaxed); }
private:
    struct Slot
    {
        std::vector<std::shared_ptr<const IGraphicsTexture>> owners;
        std::unordered_set<const IGraphicsTexture*> seen;
    };
    std::mutex m_mutex;
    std::vector<Slot> m_slots;
    size_t m_slot = 0;
    std::atomic<uint64_t> m_generation{0};
};

class IGraphicsTextureFactory
{
public:
    virtual ~IGraphicsTextureFactory() = default;
    virtual void PollUploads() {}
    virtual uint64_t GetPendingUploadBytes() const { return 0; }
    // False when the device cannot sample BC1/4/5/7; loaders then decode the
    // uncompressed source instead of a cooked texture.
    virtual bool SupportsBlockCompression() const { return true; }
    // Conservative fallback for backends without an allocation query (DX11).
    virtual uint64_t EstimateAllocationBytes(uint32_t width, uint32_t height,
        uint32_t levels, GraphicsTextureFormat format, bool = true) const
    {
        const uint64_t bytes = GraphicsTextureChainBytes(format, width, height, levels);
        return (bytes + 65535) & ~uint64_t(65535);
    }
    virtual std::shared_ptr<IGraphicsTexture> CreateTexture2D(
        uint32_t width,
        uint32_t height,
        const uint8_t* pixels,
        uint32_t mipLevels,
        GraphicsTextureFormat format,
        bool srgb = true) = 0;

    // Texture2DArray with `layers` slices of identical size and format. The
    // pixels hold each slice's full packed mip chain, slice after slice. It is
    // always viewed as an array, even with a single slice.
    virtual std::shared_ptr<IGraphicsTexture> CreateTexture2DArray(
        uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t,
        GraphicsTextureFormat, bool = true) { return nullptr; }

    // Creates a depth texture that can be used both as a depth-only render
    // target and as a pixel-shader resource. Backends that have not yet
    // implemented offscreen depth rendering return nullptr.
    virtual std::shared_ptr<IGraphicsTexture> CreateDepthTexture2D(
        uint32_t, uint32_t) { return nullptr; }
};
}
