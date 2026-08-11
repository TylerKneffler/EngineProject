#include "Texture.h"

#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <tinyexr.h>
#include <ktx.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace
{
std::unordered_map<std::string, std::weak_ptr<Texture>>& TextureRegistry()
{
    static std::unordered_map<std::string, std::weak_ptr<Texture>> registry;
    return registry;
}

std::string TextureIdentity(const std::string& path, bool srgb)
{
    std::error_code error;
    std::filesystem::path identity =
        std::filesystem::absolute(std::filesystem::path(path).lexically_normal(), error);
    if (error)
        identity = std::filesystem::path(path).lexically_normal();
    std::string key = identity.generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return key + (srgb ? "|srgb" : "|linear");
}

uint32_t FullMipCount(uint32_t width, uint32_t height)
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

float SrgbToLinear(uint8_t value)
{
    const float v = value / 255.f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

uint8_t LinearToSrgb(float value)
{
    value = std::clamp(value, 0.f, 1.f);
    const float result = value <= 0.0031308f
        ? value * 12.92f : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(result * 255.f + 0.5f, 0.f, 255.f));
}

void GenerateMipChain(std::vector<uint8_t>& pixels, uint32_t width,
    uint32_t height, GraphicsTextureFormat format, bool srgb)
{
    uint32_t sourceWidth = width;
    uint32_t sourceHeight = height;
    size_t sourceOffset = 0;
    const uint32_t bytesPerPixel = GraphicsTextureBytesPerPixel(format);
    while (sourceWidth > 1 || sourceHeight > 1)
    {
        const uint32_t targetWidth = std::max(1u, sourceWidth / 2);
        const uint32_t targetHeight = std::max(1u, sourceHeight / 2);
        const size_t targetOffset = pixels.size();
        pixels.resize(targetOffset + static_cast<size_t>(targetWidth) * targetHeight * bytesPerPixel);
        for (uint32_t y = 0; y < targetHeight; ++y)
        {
            for (uint32_t x = 0; x < targetWidth; ++x)
            {
                const size_t destination = targetOffset +
                    (static_cast<size_t>(y) * targetWidth + x) * bytesPerPixel;
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    float sum = 0.f;
                    for (uint32_t sampleY = 0; sampleY < 2; ++sampleY)
                    for (uint32_t sampleX = 0; sampleX < 2; ++sampleX)
                    {
                        const uint32_t sourceX = std::min(sourceWidth - 1, x * 2 + sampleX);
                        const uint32_t sourceY = std::min(sourceHeight - 1, y * 2 + sampleY);
                        const size_t source = sourceOffset +
                            (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * bytesPerPixel;
                        if (format == GraphicsTextureFormat::Rgba32Float)
                        {
                            float value = 0.f;
                            std::memcpy(&value, pixels.data() + source + channel * sizeof(float), sizeof(float));
                            sum += value;
                        }
                        else
                        {
                            const uint8_t value = pixels[source + channel];
                            sum += srgb && channel < 3 ? SrgbToLinear(value) : value / 255.f;
                        }
                    }
                    const float average = sum * 0.25f;
                    if (format == GraphicsTextureFormat::Rgba32Float)
                        std::memcpy(pixels.data() + destination + channel * sizeof(float), &average, sizeof(float));
                    else
                        pixels[destination + channel] = srgb && channel < 3
                            ? LinearToSrgb(average)
                            : static_cast<uint8_t>(std::clamp(average * 255.f + 0.5f, 0.f, 255.f));
                }
            }
        }
        sourceOffset = targetOffset;
        sourceWidth = targetWidth;
        sourceHeight = targetHeight;
    }
}
}

std::shared_ptr<Texture> Texture::Acquire(const std::string& path, bool srgb)
{
    const std::string key = TextureIdentity(path, srgb);
    auto& registry = TextureRegistry();
    if (auto found = registry.find(key); found != registry.end())
        if (auto existing = found->second.lock())
            return existing;

    auto texture = std::shared_ptr<Texture>(new Texture(
        std::filesystem::path(path).lexically_normal().generic_string(), srgb));
    registry[key] = texture;
    return texture;
}

void Texture::Invalidate(const std::string& path)
{
    for (const bool srgb : { false, true })
    {
        const auto found = TextureRegistry().find(TextureIdentity(path, srgb));
        if (found != TextureRegistry().end())
            if (const auto texture = found->second.lock())
                texture->InvalidateCache();
    }
}

void Texture::InvalidateCache()
{
    m_pixels.clear();
    m_width = m_height = 0;
    m_mipLevels = 0;
    m_format = GraphicsTextureFormat::Rgba8;
    m_graphicsTexture.reset();
    m_preparedProvider = nullptr;
}

bool Texture::Load()
{
    if (!m_pixels.empty())
        return true;

    std::string extension = std::filesystem::path(m_filePath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    bool success = false;
    if (extension == ".hdr")
    {
        int width = 0, height = 0, channels = 0;
        std::unique_ptr<float, decltype(&stbi_image_free)> decoded(
            stbi_loadf(m_filePath.c_str(), &width, &height, &channels, 4), stbi_image_free);
        if (decoded && width > 0 && height > 0)
        {
            m_width = static_cast<uint32_t>(width);
            m_height = static_cast<uint32_t>(height);
            m_format = GraphicsTextureFormat::Rgba32Float;
            m_pixels.resize(static_cast<size_t>(m_width) * m_height * 16);
            std::memcpy(m_pixels.data(), decoded.get(), m_pixels.size());
            success = true;
        }
    }
    else if (extension == ".exr")
    {
        float* decoded = nullptr;
        int width = 0, height = 0;
        const char* error = nullptr;
        if (LoadEXR(&decoded, &width, &height, m_filePath.c_str(), &error) == TINYEXR_SUCCESS &&
            decoded && width > 0 && height > 0)
        {
            m_width = static_cast<uint32_t>(width);
            m_height = static_cast<uint32_t>(height);
            m_format = GraphicsTextureFormat::Rgba32Float;
            m_pixels.resize(static_cast<size_t>(m_width) * m_height * 16);
            std::memcpy(m_pixels.data(), decoded, m_pixels.size());
            success = true;
        }
        free(decoded);
        if (error) FreeEXRErrorMessage(error);
    }
    else if (extension == ".tga")
    {
        int width = 0, height = 0, channels = 0;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
            stbi_load(m_filePath.c_str(), &width, &height, &channels, 4), stbi_image_free);
        if (decoded && width > 0 && height > 0)
        {
            m_width = static_cast<uint32_t>(width);
            m_height = static_cast<uint32_t>(height);
            m_pixels.assign(decoded.get(), decoded.get() + static_cast<size_t>(m_width) * m_height * 4);
            success = true;
        }
    }
    else if (extension == ".ktx2")
    {
        ktxTexture2* raw = nullptr;
        if (ktxTexture2_CreateFromNamedFile(m_filePath.c_str(),
                KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) == KTX_SUCCESS && raw)
        {
            std::unique_ptr<ktxTexture2, void(*)(ktxTexture2*)> texture(raw,
                [](ktxTexture2* value) { ktxTexture_Destroy(ktxTexture(value)); });
            const bool basis = ktxTexture2_NeedsTranscoding(texture.get());
            bool rgba = true;
            if (basis)
                rgba = ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS;
            const uint32_t format = texture->vkFormat;
            const bool bgra = !basis && (format == 44u || format == 50u);
            const bool supported = rgba && texture->numDimensions == 2 && !texture->isArray &&
                texture->numFaces == 1 && texture->baseDepth == 1 &&
                (basis || format == 37u || format == 43u || bgra);
            ktx_size_t offset = 0;
            if (supported && ktxTexture_GetImageOffset(ktxTexture(texture.get()), 0, 0, 0, &offset) == KTX_SUCCESS)
            {
                m_width = texture->baseWidth;
                m_height = texture->baseHeight;
                const size_t size = static_cast<size_t>(m_width) * m_height * 4;
                const uint8_t* source = ktxTexture_GetData(ktxTexture(texture.get())) + offset;
                m_pixels.assign(source, source + size);
                if (bgra)
                    for (size_t pixel = 0; pixel < size; pixel += 4)
                        std::swap(m_pixels[pixel], m_pixels[pixel + 2]);
                success = true;
            }
        }
    }

    if (!success && extension != ".hdr" && extension != ".exr" &&
        extension != ".tga" && extension != ".ktx2")
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
            return false;

        {
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

            const std::wstring path = std::filesystem::path(m_filePath).wstring();
            if (SUCCEEDED(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&m_width, &m_height)) &&
            m_width > 0 && m_height > 0)
            {
                const uint64_t size = static_cast<uint64_t>(m_width) * m_height * 4;
                if (size <= UINT32_MAX)
                {
                    m_pixels.resize(static_cast<size_t>(size));
                    success = SUCCEEDED(converter->CopyPixels(
                        nullptr, m_width * 4, static_cast<UINT>(size), m_pixels.data()));
                }
            }
        }

        if (uninitialize)
            CoUninitialize();
    }
    if (success)
    {
        m_mipLevels = FullMipCount(m_width, m_height);
        GenerateMipChain(m_pixels, m_width, m_height, m_format, m_srgb);
    }
    else
    {
        m_pixels.clear();
        m_width = m_height = m_mipLevels = 0;
        m_format = GraphicsTextureFormat::Rgba8;
    }
    return success;
}

void Texture::Reload()
{
    m_pixels.clear();
    m_width = m_height = 0;
    m_mipLevels = 0;
    m_format = GraphicsTextureFormat::Rgba8;
    m_graphicsTexture.reset();
    m_preparedProvider = nullptr;
}

bool Texture::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        return false;
    if (m_graphicsTexture && m_preparedProvider == graphicsProvider)
        return true;
    if (!Load())
        return false;
    IGraphicsTextureFactory* factory = graphicsProvider->GetTextureFactory();
    if (!factory)
        return false;
    try
    {
        m_graphicsTexture =
            factory->CreateTexture2D(
                m_width, m_height, m_pixels.data(), m_mipLevels, m_format, m_srgb);
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(("[Texture] GPU upload failed for " + m_filePath +
            ": " + error.what() + "\n").c_str());
        m_graphicsTexture.reset();
    }
    m_preparedProvider = m_graphicsTexture ? graphicsProvider : nullptr;
    return static_cast<bool>(m_graphicsTexture);
}
