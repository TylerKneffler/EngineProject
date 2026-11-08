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
    Rgba32Float
};

inline uint32_t GraphicsTextureBytesPerPixel(GraphicsTextureFormat format)
{
    return format == GraphicsTextureFormat::Rgba32Float ? 16u : 4u;
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
    // Conservative fallback for backends without an allocation query (DX11).
    virtual uint64_t EstimateAllocationBytes(uint32_t width, uint32_t height,
        uint32_t levels, GraphicsTextureFormat format, bool = true) const
    {
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < levels; ++i)
        {
            bytes += uint64_t(width) * height * GraphicsTextureBytesPerPixel(format);
            width = std::max(1u, width / 2); height = std::max(1u, height / 2);
        }
        return (bytes + 65535) & ~uint64_t(65535);
    }
    virtual std::shared_ptr<IGraphicsTexture> CreateTexture2D(
        uint32_t width,
        uint32_t height,
        const uint8_t* pixels,
        uint32_t mipLevels,
        GraphicsTextureFormat format,
        bool srgb = true) = 0;

    // Creates a depth texture that can be used both as a depth-only render
    // target and as a pixel-shader resource. Backends that have not yet
    // implemented offscreen depth rendering return nullptr.
    virtual std::shared_ptr<IGraphicsTexture> CreateDepthTexture2D(
        uint32_t, uint32_t) { return nullptr; }
};
}
