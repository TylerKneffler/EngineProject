#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Core/Graphics/IGraphicsTexture.h"

namespace Engine::Graphics { class IGraphicsProvider; }

namespace Engine::Components
{
class Texture
{
public:
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    using IGraphicsTexture = Engine::Graphics::IGraphicsTexture;
    using GraphicsTextureFormat = Engine::Graphics::GraphicsTextureFormat;

    static std::shared_ptr<Texture> Acquire(
        const std::string& path, bool srgb = true);
    // Discard decoded/GPU data for a generated asset that was overwritten.
    // Existing shared references will lazily reload it on the next Prepare.
    static void Invalidate(const std::string& path);

    bool Load();
    // Discards CPU/GPU copies so a rewritten generated texture is visible on
    // the next Prepare call.
    void Reload();
    bool Prepare(IGraphicsProvider* graphicsProvider);

    const std::string& GetFilePath() const { return m_filePath; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    uint32_t GetMipLevels() const { return m_mipLevels; }
    GraphicsTextureFormat GetFormat() const { return m_format; }
    bool HasPixels() const { return !m_pixels.empty(); }
    const std::vector<uint8_t>& GetPixels() const { return m_pixels; }
    bool IsSrgb() const { return m_srgb; }
    const IGraphicsTexture* GetGraphicsTexture() const { return m_graphicsTexture.get(); }

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
    IGraphicsProvider* m_preparedProvider = nullptr;
    std::shared_ptr<IGraphicsTexture> m_graphicsTexture;
};
}
