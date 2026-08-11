#pragma once

#include <cstdint>
#include <memory>

enum class GraphicsTextureFormat
{
    Rgba8,
    Rgba32Float
};

inline uint32_t GraphicsTextureBytesPerPixel(GraphicsTextureFormat format)
{
    return format == GraphicsTextureFormat::Rgba32Float ? 16u : 4u;
}

class IGraphicsTexture
{
public:
    virtual ~IGraphicsTexture() = default;
    virtual void* GetNativeHandle() const = 0;
};

class IGraphicsTextureFactory
{
public:
    virtual ~IGraphicsTextureFactory() = default;
    virtual std::shared_ptr<IGraphicsTexture> CreateTexture2D(
        uint32_t width,
        uint32_t height,
        const uint8_t* pixels,
        uint32_t mipLevels,
        GraphicsTextureFormat format,
        bool srgb = true) = 0;
};
