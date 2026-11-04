#pragma once

#include <cstdint>
#include <memory>
#include <future>
#include <string>
#include <vector>

#include "Core/Graphics/IGraphicsTexture.h"

namespace Engine::Graphics { class IGraphicsProvider; }

namespace Engine::Components
{
class Texture : public std::enable_shared_from_this<Texture>
{
public:
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    using IGraphicsTexture = Engine::Graphics::IGraphicsTexture;
    using GraphicsTextureFormat = Engine::Graphics::GraphicsTextureFormat;

    static std::shared_ptr<Texture> Acquire(
        const std::string& path, bool srgb = true);
    // Worker-owned CPU decoding, independent of cached/live render textures.
    // Thumbnails sample only the base level and do not need a mip chain.
    static std::shared_ptr<Texture> DecodeDetached(
        const std::string& path, bool srgb = true);
    // Discard decoded/GPU data for a generated asset that was overwritten.
    // Existing shared references will lazily reload it on the next Prepare.
    static void Invalidate(const std::string& path);

    bool Load();
    // Discards CPU/GPU copies so a rewritten generated texture is visible on
    // the next Prepare call.
    void Reload();
    // Replaces the decoded RGBA8 image during an editor paint stroke. The
    // caller persists the pixels separately when the stroke is committed.
    bool SetPreviewPixels(const std::vector<uint8_t>& rgba);
    bool Prepare(IGraphicsProvider* graphicsProvider);
    // Startup-only path for resources required before the first frame.
    bool PrepareImmediate(IGraphicsProvider* graphicsProvider);
    // Schedule CPU decoding; publication is pumped on the graphics owner.
    void QueuePrepare(IGraphicsProvider* graphicsProvider);
    static void PublishQueued(IGraphicsProvider* graphicsProvider, size_t maxUploads = 2);
    void RetainPixelsForCpu() { m_retainPixelsForCpu = true; }

    const std::string& GetFilePath() const { return m_filePath; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetMipLevels() const { return m_mipLevels; }
    GraphicsTextureFormat GetFormat() const { return m_format; }
    bool HasPixels() const { return !m_pixels.empty(); }
    // Decode on demand for editor tools after the render upload has released
    // its CPU copy.
    const std::vector<uint8_t>& GetPixels() const;
    bool IsSrgb() const { return m_srgb; }
    const IGraphicsTexture* GetGraphicsTexture() const { return m_graphicsTexture.get(); }
    bool IsPreparedFor(const IGraphicsProvider* provider) const
    {
        return provider && m_preparedProvider==provider && m_graphicsTexture &&
            m_graphicsTexture->GetNativeHandle();
    }

private:
    explicit Texture(std::string path, bool srgb)
        : m_filePath(std::move(path)), m_srgb(srgb) {}
    void InvalidateCache();

    std::string m_filePath;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_mipLevels = 0;
    std::vector<uint8_t> m_pixels;
    GraphicsTextureFormat m_format = GraphicsTextureFormat::Rgba8;
    bool m_srgb = true;
    bool m_generateMips = true;
    bool m_retainPixelsForCpu = false;
    IGraphicsProvider* m_preparedProvider = nullptr;
    std::shared_ptr<IGraphicsTexture> m_graphicsTexture;
    std::future<std::shared_ptr<Texture>> m_decode;
    bool m_queued = false;
    IGraphicsProvider* m_queuedProvider = nullptr;
    bool m_decodeFailed = false;
    uint64_t m_generation = 0;
};
}
