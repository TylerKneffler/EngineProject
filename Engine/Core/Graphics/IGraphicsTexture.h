#pragma once

#include <cstdint>
#include <memory>

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

    // Creates a depth texture that can be used both as a depth-only render
    // target and as a pixel-shader resource. Backends that have not yet
    // implemented offscreen depth rendering return nullptr.
    virtual std::shared_ptr<IGraphicsTexture> CreateDepthTexture2D(
        uint32_t, uint32_t) { return nullptr; }
};
}

