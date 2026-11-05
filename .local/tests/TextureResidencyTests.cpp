#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Engine::Graphics;
using Engine::Components::Texture;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Handle : IGraphicsTexture
{
    void* GetNativeHandle() const override { return const_cast<Handle*>(this); }
};

struct Provider : IGraphicsProvider, IGraphicsTextureFactory
{
    uint32_t width = 0, height = 0, levels = 0, calls = 0;
    std::vector<uint8_t> uploaded;
    IShaderCompiler* GetShaderCompiler() override { return nullptr; }
    IGraphicsBufferFactory* GetBufferFactory() override { return nullptr; }
    IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
    IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
    IGraphicsTextureFactory* GetTextureFactory() override { return this; }
    std::shared_ptr<IGraphicsTexture> CreateTexture2D(uint32_t w, uint32_t h,
        const uint8_t* pixels, uint32_t mips, GraphicsTextureFormat format, bool) override
    {
        width = w; height = h; levels = mips; ++calls;
        size_t bytes = 0;
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            bytes += size_t(w) * h * GraphicsTextureBytesPerPixel(format);
            w = std::max(1u, w / 2); h = std::max(1u, h / 2);
        }
        uploaded.assign(pixels, pixels + bytes);
        return std::make_shared<Handle>();
    }
};

int main(int argc, char** argv)
{
    try
    {
        Check(argc == 3, "expected texture path and effective cap");
        const uint32_t cap = static_cast<uint32_t>(std::stoul(argv[2]));
        auto texture = Texture::Acquire(argv[1]);
        Check(texture->Load(), "decode failed");
        const uint32_t sourceWidth = texture->GetWidth(), sourceHeight = texture->GetHeight();
        const uint32_t sourceLevels = texture->GetMipLevels();
        const auto original = texture->GetPixels();
        uint32_t w = sourceWidth, h = sourceHeight, levels = sourceLevels;
        size_t offset = 0;
        while (cap && std::max(w, h) > cap && levels > 1)
        {
            offset += size_t(w) * h * 4;
            w = std::max(1u, w / 2); h = std::max(1u, h / 2); --levels;
        }
        const std::vector<uint8_t> expected(original.begin() + offset, original.end());
        Provider first, second;
        auto verify = [&](Provider& provider) {
            Check(texture->PrepareImmediate(&provider), "upload failed");
            Check(provider.width == w && provider.height == h && provider.levels == levels,
                "wrong upload dimensions or levels");
            Check(provider.uploaded == expected, "wrong mip bytes or offset");
            Check(texture->GetWidth() == sourceWidth && texture->GetHeight() == sourceHeight &&
                texture->GetMipLevels() == sourceLevels, "source metadata changed");
        };
        verify(first);
        verify(first);
        Check(first.calls == 1, "prepared texture uploaded twice");
        Check(texture->GetPixels() == original, "CPU pixels lost source resolution");
        verify(second);
        texture->Reload();
        verify(second);
        Check(second.calls == 2, "reload did not recreate resource");
        std::cout << "Resident cap verified: " << w << 'x' << h << ", " << levels << " mips\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
