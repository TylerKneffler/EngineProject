#include "Core/Compoonents/Materials/Texture.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
uint32_t ExpectedMipCount(uint32_t width, uint32_t height)
{
    uint32_t count = 1;
    while (width > 1 || height > 1)
    {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++count;
    }
    return count;
}

size_t ExpectedByteCount(uint32_t width, uint32_t height, uint32_t mipLevels,
    GraphicsTextureFormat format)
{
    size_t result = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        result += static_cast<size_t>(width) * height *
            GraphicsTextureBytesPerPixel(format);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return result;
}

bool Check(const char* label, const char* path, GraphicsTextureFormat expectedFormat)
{
    const auto texture = Texture::Acquire(path);
    if (!texture->Load())
    {
        std::cerr << label << " failed to decode: " << path << '\n';
        return false;
    }
    const uint32_t expectedMips = ExpectedMipCount(
        texture->GetWidth(), texture->GetHeight());
    if (!texture->GetWidth() || !texture->GetHeight() ||
        texture->GetMipLevels() != expectedMips ||
        texture->GetFormat() != expectedFormat ||
        texture->GetPixels().size() != ExpectedByteCount(
            texture->GetWidth(), texture->GetHeight(), expectedMips, expectedFormat))
    {
        std::cerr << label << " decoded with invalid dimensions, format, or mip chain\n";
        return false;
    }
    std::cout << label << ": " << texture->GetWidth() << 'x' << texture->GetHeight()
              << ", " << texture->GetMipLevels() << " mips\n";
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::cerr << "Expected TGA, HDR, EXR, and KTX2 fixture paths\n";
        return 2;
    }
    const bool success =
        Check("TGA", argv[1], GraphicsTextureFormat::Rgba8) &&
        Check("HDR", argv[2], GraphicsTextureFormat::Rgba32Float) &&
        Check("EXR", argv[3], GraphicsTextureFormat::Rgba32Float) &&
        Check("KTX2", argv[4], GraphicsTextureFormat::Rgba8);
    return success ? 0 : 1;
}
