#pragma once

#include <cstdint>
#include <memory>

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
        const uint8_t* rgbaPixels,
        bool srgb = true) = 0;
};
