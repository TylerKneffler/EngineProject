// Loads TextureCook output through the engine Texture path and checks the
// block-compressed formats, stored mip chains and the CPU (editor) decode.
//
//   TextureCompressionTests <cooked dir> <source dir>
//
// Expects CopperSlate_{BaseColor,Normal,Height,MetallicRoughness}.ktx2 in the
// cooked dir and the original PNGs in the source dir.

#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsTexture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using Engine::Components::Texture;
using Engine::Graphics::GraphicsTextureFormat;
namespace G = Engine::Graphics;

namespace
{
int failures = 0;
void Check(bool condition, const char* what)
{
    if (!condition) { ++failures; std::printf("FAIL: %s\n", what); }
}

struct Mean { double r = 0, g = 0, b = 0; };
Mean Average(const std::vector<uint8_t>& rgba, size_t texels)
{
    Mean mean;
    for (size_t i = 0; i < texels; ++i)
    {
        mean.r += rgba[i * 4]; mean.g += rgba[i * 4 + 1]; mean.b += rgba[i * 4 + 2];
    }
    mean.r /= texels; mean.g /= texels; mean.b /= texels;
    return mean;
}

void CheckCooked(const std::string& cooked, const std::string& source, bool srgb,
    GraphicsTextureFormat expected, const char* name)
{
    std::printf("%s\n", name);
    auto texture = Texture::Acquire(cooked, srgb);
    Check(texture->Load(), "render load");
    Check(texture->HasPixels(), "render load keeps the block data");
    Check(texture->GetFormat() == expected, "block format");
    Check(texture->GetWidth() == 1024 && texture->GetHeight() == 1024, "power-of-two size");
    Check(texture->GetMipLevels() == 11, "full stored mip chain");

    // CPU readback decodes BC to RGBA8 (level 0 only is guaranteed).
    const auto& pixels = texture->GetPixels();
    const size_t texels = size_t(texture->GetWidth()) * texture->GetHeight();
    Check(pixels.size() >= texels * 4, "CPU decode size");
    if (pixels.size() < texels * 4) return;

    // Compression and the 1254 -> 1024 resample keep the image mean.
    auto original = Texture::DecodeDetached(source, srgb);
    Check(original && original->GetFormat() == GraphicsTextureFormat::Rgba8, "source decode");
    if (!original) return;
    const Mean a = Average(pixels, texels);
    const Mean b = Average(original->GetPixels(),
        size_t(original->GetWidth()) * original->GetHeight());
    const double error = std::max({std::abs(a.r - b.r), std::abs(a.g - b.g),
        expected == GraphicsTextureFormat::Bc7 ? std::abs(a.b - b.b) : 0.0});
    std::printf("  mean error %.2f / 255\n", error);
    Check(error < 3.0, "mean preserved");
}
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: TextureCompressionTests <cooked dir> <source dir>\n"); return 2; }
    const std::string cooked = std::string(argv[1]) + "/CopperSlate_";
    const std::string source = std::string(argv[2]) + "/CopperSlate_";

    // Block size math used by every backend uploader and streaming.
    Check(G::GraphicsTextureMipBytes(GraphicsTextureFormat::Bc7, 1, 1) == 16, "BC7 1x1 is one block");
    Check(G::GraphicsTextureMipBytes(GraphicsTextureFormat::Bc4, 5, 5) == 32, "BC4 5x5 is 2x2 blocks");
    Check(G::GraphicsTextureRowBytes(GraphicsTextureFormat::Bc5, 1024) == 4096, "BC5 row pitch");
    Check(G::GraphicsTextureChainBytes(GraphicsTextureFormat::Rgba8, 4, 4, 3) == 64 + 16 + 4, "RGBA8 chain");
    Check(!G::GraphicsTextureIsValidBase(GraphicsTextureFormat::Bc7, 314, 314), "odd BC base rejected");
    Check(G::GraphicsTextureIsValidBase(GraphicsTextureFormat::Rgba8, 314, 314), "RGBA any base");

    CheckCooked(cooked + "BaseColor.ktx2", source + "BaseColor.png", true,
        GraphicsTextureFormat::Bc7, "BaseColor (BC7 sRGB)");
    CheckCooked(cooked + "Height.ktx2", source + "Height.png", false,
        GraphicsTextureFormat::Bc4, "Height (BC4)");
    CheckCooked(cooked + "Normal.ktx2", source + "Normal.png", false,
        GraphicsTextureFormat::Bc5, "Normal (BC5)");
    CheckCooked(cooked + "MetallicRoughness.ktx2", source + "MetallicRoughness.png", false,
        GraphicsTextureFormat::Bc7, "MetallicRoughness (BC7 linear, Toksvig)");

    // BC4 decodes to grey; BC5 rebuilds a unit Z.
    {
        const auto& height = Texture::Acquire(cooked + "Height.ktx2", false)->GetPixels();
        Check(height.size() >= 4 && height[0] == height[1] && height[1] == height[2], "BC4 grey decode");
        auto normalTexture = Texture::Acquire(cooked + "Normal.ktx2", false);
        const auto& normal = normalTexture->GetPixels();
        // Level 0 only: the CPU copy's generated mips hold averaged (shorter) normals.
        const size_t level0 = size_t(normalTexture->GetWidth()) * normalTexture->GetHeight() * 4;
        double worst = 0;
        for (size_t i = 0; i + 3 < std::min(level0, normal.size()); i += 4 * 97)
        {
            const double x = normal[i] / 127.5 - 1, y = normal[i + 1] / 127.5 - 1, z = normal[i + 2] / 127.5 - 1;
            worst = std::max(worst, std::abs(std::sqrt(x * x + y * y + z * z) - 1.0));
        }
        std::printf("  BC5 worst |n|-1: %.3f\n", worst);
        Check(worst < 0.05, "BC5 normals unit length");
    }

    // Uncompressed sources keep the existing RGBA8 path with generated mips.
    {
        auto png = Texture::Acquire(source + "Height.png", false);
        Check(png->Load() && png->GetFormat() == GraphicsTextureFormat::Rgba8, "PNG stays RGBA8");
        Check(png->GetMipLevels() == 11, "PNG generated mips (1254 -> 11 levels)");
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
