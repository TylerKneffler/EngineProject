#include "Texture.h"

#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Memory/CacheStore.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/TextureResidency.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <tinyexr.h>
#include <ktx.h>
#include <bc7decomp.h>
#include <rgbcx.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <memory>
#include <deque>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/geometric.hpp>

namespace Engine::Components
{
namespace
{
std::mutex s_publicationMutex;
std::deque<std::pair<std::weak_ptr<Texture>, uint64_t>> s_publications;
std::mutex s_textureRegistryMutex;
std::vector<std::weak_ptr<Texture>> s_textureRegistry;
struct GraphicsTextureOwner { std::weak_ptr<Engine::Graphics::IGraphicsTexture> resource; std::weak_ptr<Texture> texture; };
std::unordered_map<const Engine::Graphics::IGraphicsTexture*,GraphicsTextureOwner> s_graphicsTextureOwners;
std::mutex s_graphicsTextureOwnersMutex;
Texture::StreamingSnapshot s_streamSnapshot;
std::mutex s_streamStatsMutex;
uint64_t s_streamUploads = 0, s_streamEvictions = 0;
struct ProviderStreamingFrame { uint64_t serial=0, frame=0, uploaded=0; };
std::unordered_map<const Engine::Graphics::IGraphicsProvider*,ProviderStreamingFrame> s_providerStreamingFrames;
Engine::Rendering::TextureStreamingSettings& StreamingSettings()
{
    static auto settings = [] {
        Engine::Rendering::TextureStreamingSettings result;
        if (const char* text = std::getenv("ENGINE_TEXTURE_STREAMING_BUDGET_MB"))
        {
            uint32_t mb = 0;
            const std::string_view value(text);
            const auto parsed = std::from_chars(value.data(),value.data()+value.size(),mb);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data()+value.size())
                result.budgetBytes = uint64_t(mb) * 1024 * 1024;
        }
        return result;
    }();
    return settings;
}

uint32_t ResidentTextureDimensionCap()
{
    // One process-wide startup policy keeps shared cache entries consistent.
    static const uint32_t cap = [] {
        const char* text = std::getenv("ENGINE_TEXTURE_MAX_DIMENSION");
        if (!text) return 0u;
        const std::string_view value(text);
        uint32_t parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size()
            ? parsed : 0u;
    }();
    return cap;
}
std::string TextureIdentity(const std::string& path, bool srgb)
{
    return Engine::Memory::CacheStore::PathKey(path,
        srgb ? "srgb" : "linear");
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

// Native block-compressed KTX2 payloads the GPU samples directly.
bool KtxBlockFormat(uint32_t vkFormat, Engine::Graphics::GraphicsTextureFormat& format)
{
    using Engine::Graphics::GraphicsTextureFormat;
    switch (vkFormat)
    {
    case 131u: case 132u: case 133u: case 134u: format = GraphicsTextureFormat::Bc1; return true;
    case 139u: format = GraphicsTextureFormat::Bc4; return true;
    case 141u: format = GraphicsTextureFormat::Bc5; return true;
    case 145u: case 146u: format = GraphicsTextureFormat::Bc7; return true;
    default: return false;
    }
}

// CPU decode of one block-compressed level to RGBA8 for editor/baking reads.
// One-channel data is replicated to grey; BC5 normals get Z rebuilt.
std::vector<uint8_t> DecodeBlockLevel(Engine::Graphics::GraphicsTextureFormat format,
    const uint8_t* blocks, uint32_t width, uint32_t height)
{
    using Engine::Graphics::GraphicsTextureFormat;
    static std::once_flag rgbcxInit;
    std::call_once(rgbcxInit, [] { rgbcx::init(); });
    const uint32_t blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
    const uint32_t blockBytes = Engine::Graphics::GraphicsTextureBytesPerBlock(format);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (uint32_t by = 0; by < blocksHigh; ++by)
    for (uint32_t bx = 0; bx < blocksWide; ++bx)
    {
        const uint8_t* block = blocks + (static_cast<size_t>(by) * blocksWide + bx) * blockBytes;
        uint8_t texels[16 * 4];
        std::memset(texels, 255, sizeof(texels));
        switch (format)
        {
        case GraphicsTextureFormat::Bc1: rgbcx::unpack_bc1(block, texels, true); break;
        case GraphicsTextureFormat::Bc4:
            rgbcx::unpack_bc4(block, texels, 4);
            for (uint32_t i = 0; i < 16; ++i) texels[i * 4 + 1] = texels[i * 4 + 2] = texels[i * 4];
            break;
        case GraphicsTextureFormat::Bc5:
            rgbcx::unpack_bc5(block, texels, 0, 1, 4);
            for (uint32_t i = 0; i < 16; ++i)
            {
                const float x = texels[i * 4] / 127.5f - 1.f, y = texels[i * 4 + 1] / 127.5f - 1.f;
                const float z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
                texels[i * 4 + 2] = static_cast<uint8_t>(std::clamp(z * 127.5f + 127.5f, 0.f, 255.f));
            }
            break;
        default:
            bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(texels));
            break;
        }
        for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
            std::memcpy(rgba.data() + ((static_cast<size_t>(by) * 4 + y) * width + bx * 4 + x) * 4,
                texels + (y * 4 + x) * 4, 4);
    }
    return rgba;
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
    uint32_t height, Engine::Graphics::GraphicsTextureFormat format, bool srgb)
{
    uint32_t sourceWidth = width;
    uint32_t sourceHeight = height;
    size_t sourceOffset = 0;
    const uint32_t bytesPerPixel = Engine::Graphics::GraphicsTextureBytesPerPixel(format);
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
                        if (format == Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
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
                    if (format == Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
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

float RadicalInverseVdc(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

glm::vec3 ReadFloatPanorama(const std::vector<uint8_t>& pixels,
    uint32_t width, uint32_t height, const glm::vec3& direction)
{
    constexpr float inverseTwoPi = 0.15915494309189535f;
    constexpr float inversePi = 0.3183098861837907f;
    float u = 0.5f + std::atan2(direction.z, direction.x) * inverseTwoPi;
    u -= std::floor(u);
    const float v = std::acos(std::clamp(direction.y, -1.f, 1.f)) * inversePi;
    const uint32_t x = std::min(width - 1u,
        static_cast<uint32_t>(u * static_cast<float>(width)));
    const uint32_t y = std::min(height - 1u,
        static_cast<uint32_t>(v * static_cast<float>(height)));
    const size_t offset = (static_cast<size_t>(y) * width + x) * 16u;
    glm::vec3 value{};
    std::memcpy(&value.x, pixels.data() + offset, sizeof(float));
    std::memcpy(&value.y, pixels.data() + offset + 4u, sizeof(float));
    std::memcpy(&value.z, pixels.data() + offset + 8u, sizeof(float));
    return glm::max(value, glm::vec3(0.f));
}

void PrefilterGgxEnvironmentMips(std::vector<uint8_t>& pixels,
    uint32_t width, uint32_t height, uint32_t mipLevels)
{
    if (!width || !height || mipLevels < 2u)
        return;
    constexpr uint32_t sampleCount = 64u;
    constexpr float twoPi = 6.28318530717958647692f;
    uint32_t uploadBaseMip = 0u;
    uint32_t uploadWidth = width;
    uint32_t uploadHeight = height;
    while (std::max(uploadWidth, uploadHeight) > 2048u &&
        uploadBaseMip + 1u < mipLevels)
    {
        uploadWidth = std::max(1u, uploadWidth / 2u);
        uploadHeight = std::max(1u, uploadHeight / 2u);
        ++uploadBaseMip;
    }
    size_t mipOffset = static_cast<size_t>(width) * height * 16u;
    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t mip = 1u; mip < mipLevels; ++mip)
    {
        mipWidth = std::max(1u, mipWidth / 2u);
        mipHeight = std::max(1u, mipHeight / 2u);
        const size_t mipBytes = static_cast<size_t>(mipWidth) * mipHeight * 16u;
        if (mipWidth <= 512u)
        {
            const float roughness = static_cast<float>(
                mip > uploadBaseMip ? mip - uploadBaseMip : 0u) /
                static_cast<float>(std::max(1u,
                    mipLevels - uploadBaseMip - 1u));
            const float alpha = std::max(roughness * roughness, 0.001f);
            const float alphaSquared = alpha * alpha;
            for (uint32_t y = 0; y < mipHeight; ++y)
            for (uint32_t x = 0; x < mipWidth; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) /
                    static_cast<float>(mipWidth);
                const float v = (static_cast<float>(y) + 0.5f) /
                    static_cast<float>(mipHeight);
                const float phi = (u - 0.5f) * twoPi;
                const float theta = v * 3.14159265358979323846f;
                const glm::vec3 normal(std::sin(theta) * std::cos(phi),
                    std::cos(theta), std::sin(theta) * std::sin(phi));
                const glm::vec3 tangent = std::abs(normal.y) < 0.999f
                    ? glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), normal))
                    : glm::vec3(1.f, 0.f, 0.f);
                const glm::vec3 bitangent = glm::cross(normal, tangent);
                glm::vec3 radiance(0.f);
                float weight = 0.f;
                for (uint32_t sample = 0; sample < sampleCount; ++sample)
                {
                    const float xiX = static_cast<float>(sample) /
                        static_cast<float>(sampleCount);
                    const float xiY = RadicalInverseVdc(sample);
                    const float samplePhi = twoPi * xiX;
                    const float cosTheta = std::sqrt(std::max(0.f,
                        (1.f - xiY) /
                        (1.f + (alphaSquared - 1.f) * xiY)));
                    const float sinTheta = std::sqrt(std::max(0.f,
                        1.f - cosTheta * cosTheta));
                    const glm::vec3 halfVector = glm::normalize(
                        tangent * (std::cos(samplePhi) * sinTheta) +
                        bitangent * (std::sin(samplePhi) * sinTheta) +
                        normal * cosTheta);
                    const glm::vec3 light = glm::normalize(
                        2.f * glm::dot(normal, halfVector) * halfVector - normal);
                    const float noL = std::max(glm::dot(normal, light), 0.f);
                    if (noL > 0.f)
                    {
                        radiance += ReadFloatPanorama(
                            pixels, width, height, light) * noL;
                        weight += noL;
                    }
                }
                radiance /= std::max(weight, 0.0001f);
                const size_t offset = mipOffset +
                    (static_cast<size_t>(y) * mipWidth + x) * 16u;
                const float alphaChannel = 1.f;
                std::memcpy(pixels.data() + offset, &radiance.x, sizeof(float));
                std::memcpy(pixels.data() + offset + 4u, &radiance.y, sizeof(float));
                std::memcpy(pixels.data() + offset + 8u, &radiance.z, sizeof(float));
                std::memcpy(pixels.data() + offset + 12u,
                    &alphaChannel, sizeof(float));
            }
        }
        mipOffset += mipBytes;
    }
}
}

std::shared_ptr<Texture> Texture::Acquire(const std::string& path, bool srgb)
{
    const std::string key = TextureIdentity(path, srgb);
    return Engine::Memory::CacheStore::Get().GetOrCreate<Texture>(
        Engine::Memory::CacheLifetime::LongTerm, "Texture", key,
        [&]()
        {
            auto texture = std::shared_ptr<Texture>(new Texture(
                std::filesystem::path(path).lexically_normal().generic_string(), srgb));
            std::lock_guard lock(s_textureRegistryMutex);
            s_textureRegistry.push_back(texture);
            return texture;
        });
}

std::shared_ptr<Texture> Texture::DecodeDetached(const std::string& path, bool srgb)
{
    auto decoded=std::shared_ptr<Texture>(new Texture(
        std::filesystem::path(path).lexically_normal().generic_string(),srgb));
    decoded->m_generateMips=false;
    decoded->m_cpuRgba=true;
    return decoded->Load() ? decoded:nullptr;
}

void Texture::Invalidate(const std::string& path)
{
    Engine::Rendering::EnvironmentMap::Invalidate(path);
    Engine::Memory::CacheStore::Get().Erase("Lighting.EnvironmentSH",
        Engine::Memory::CacheStore::PathKey(path));
    for (const bool srgb : { false, true })
    {
        if (const auto texture = Engine::Memory::CacheStore::Get().Find<Texture>(
            "Texture", TextureIdentity(path, srgb)))
            texture->InvalidateCache();
    }
}

void Texture::InvalidateCache()
{
    m_pendingTexture.reset(); m_fallbackTexture.reset();
    m_streamDecode = {}; m_streamDecoded.reset(); m_mipHistory.Reset();
    m_managed = false; m_projectedPixels = 0.f;
    m_forceFull=false;m_budgetDenied=false;
    m_mipAllocations.clear();
    ++m_generation;
    m_decode = {};
    m_queued = false;
    m_queuedProvider = nullptr;
    m_decodeFailed = false;
    m_pixels.clear();
    m_cpuCopy.reset();
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
    bool storedMips = false;
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
    // MMD sphere maps commonly store BMP image data under .sph or .spa.
    // stb_image identifies the image from its header, independent of the suffix.
    else if (extension == ".tga" || extension == ".sph" || extension == ".spa")
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
            const bool plain2d = texture->numDimensions == 2 && !texture->isArray &&
                texture->numFaces == 1 && texture->baseDepth == 1;
            GraphicsTextureFormat blockFormat = GraphicsTextureFormat::Rgba8;
            // Render textures keep (or transcode to) BC so the GPU samples the
            // compressed data. Basis always goes to BC7: its BC4/BC5 targets
            // read G from separate slices and only suit 2-channel encodes.
            // CPU consumers (thumbnails, baking, paint tips) need RGBA8.
            bool compressed = false;
            if (basis)
                compressed = !m_cpuRgba && plain2d &&
                    Engine::Graphics::GraphicsTextureIsValidBase(GraphicsTextureFormat::Bc7,
                        texture->baseWidth, texture->baseHeight) &&
                    ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_BC7_RGBA, 0) == KTX_SUCCESS;
            if (compressed) blockFormat = GraphicsTextureFormat::Bc7;
            else if (!basis && KtxBlockFormat(texture->vkFormat, blockFormat))
                compressed = !m_cpuRgba && plain2d &&
                    Engine::Graphics::GraphicsTextureIsValidBase(blockFormat,
                        texture->baseWidth, texture->baseHeight);
            if (compressed)
            {
                const uint32_t levels = std::max(1u, texture->numLevels);
                uint64_t total = Engine::Graphics::GraphicsTextureChainBytes(
                    blockFormat, texture->baseWidth, texture->baseHeight, levels);
                std::vector<uint8_t> chain;
                chain.reserve(static_cast<size_t>(total));
                uint32_t w = texture->baseWidth, h = texture->baseHeight;
                bool complete = true;
                for (uint32_t level = 0; level < levels && complete; ++level)
                {
                    ktx_size_t offset = 0;
                    const size_t expected = static_cast<size_t>(
                        Engine::Graphics::GraphicsTextureMipBytes(blockFormat, w, h));
                    complete = ktxTexture_GetImageOffset(ktxTexture(texture.get()), level, 0, 0, &offset) == KTX_SUCCESS &&
                        ktxTexture_GetImageSize(ktxTexture(texture.get()), level) == expected;
                    if (complete)
                    {
                        const uint8_t* source = ktxTexture_GetData(ktxTexture(texture.get())) + offset;
                        chain.insert(chain.end(), source, source + expected);
                    }
                    w = std::max(1u, w / 2); h = std::max(1u, h / 2);
                }
                if (complete)
                {
                    m_width = texture->baseWidth;
                    m_height = texture->baseHeight;
                    m_format = blockFormat;
                    m_mipLevels = m_generateMips ? levels : 1u;
                    if (!m_generateMips)
                        chain.resize(static_cast<size_t>(Engine::Graphics::GraphicsTextureMipBytes(
                            blockFormat, m_width, m_height)));
                    m_pixels = std::move(chain);
                    storedMips = true;
                    success = true;
                }
            }
            if (!compressed && !basis && m_cpuRgba && plain2d &&
                KtxBlockFormat(texture->vkFormat, blockFormat))
            {
                ktx_size_t offset = 0;
                if (ktxTexture_GetImageOffset(ktxTexture(texture.get()), 0, 0, 0, &offset) == KTX_SUCCESS &&
                    ktxTexture_GetImageSize(ktxTexture(texture.get()), 0) ==
                        Engine::Graphics::GraphicsTextureMipBytes(blockFormat,
                            texture->baseWidth, texture->baseHeight))
                {
                    m_width = texture->baseWidth;
                    m_height = texture->baseHeight;
                    m_pixels = DecodeBlockLevel(blockFormat,
                        ktxTexture_GetData(ktxTexture(texture.get())) + offset, m_width, m_height);
                    success = true;
                }
            }
            bool rgba = !compressed && !success;
            if (rgba && basis)
                rgba = ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS;
            const uint32_t format = texture->vkFormat;
            const bool bgra = !basis && (format == 44u || format == 50u);
            const bool supported = rgba && plain2d &&
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
        extension != ".tga" && extension != ".sph" &&
        extension != ".spa" && extension != ".ktx2")
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
        if (!storedMips)
        {
            m_mipLevels = m_generateMips ? FullMipCount(m_width, m_height):1;
            if(m_generateMips) GenerateMipChain(m_pixels, m_width, m_height, m_format, m_srgb);
        }
    }
    else
    {
        m_pixels.clear();
        m_width = m_height = m_mipLevels = 0;
        m_format = GraphicsTextureFormat::Rgba8;
    }
    return success;
}

const std::vector<uint8_t>& Texture::GetPixels() const
{
    using Engine::Graphics::GraphicsTextureIsBlockCompressed;
    if (!GraphicsTextureIsBlockCompressed(m_format))
    {
        if (m_pixels.empty())
            const_cast<Texture*>(this)->Load();
        if (!GraphicsTextureIsBlockCompressed(m_format))
            return m_pixels;
    }
    // The render copy is block-compressed; editor callers expect RGBA8.
    // Basis sources transcode to RGBA; native BC files have no CPU decoder
    // and yield an empty image, which callers already treat as unavailable.
    if (!m_cpuCopy)
    {
        auto copy = std::shared_ptr<Texture>(new Texture(m_filePath, m_srgb));
        copy->m_cpuRgba = true;
        copy->Load();
        m_cpuCopy = std::move(copy);
    }
    return m_cpuCopy->m_pixels;
}

void Texture::Reload()
{
    Engine::Rendering::EnvironmentMap::Invalidate(m_filePath);
    Engine::Memory::CacheStore::Get().Erase("Lighting.EnvironmentSH",
        Engine::Memory::CacheStore::PathKey(m_filePath));
    InvalidateCache();
}

bool Texture::SetPreviewPixels(const std::vector<uint8_t>& rgba)
{
    if (!Load() || m_format != GraphicsTextureFormat::Rgba8 ||
        rgba.size() != static_cast<size_t>(m_width) * m_height * 4)
        return false;
    m_pixels = rgba;
    m_pendingTexture.reset(); m_fallbackTexture.reset();
    m_streamDecode={}; m_streamDecoded.reset(); m_mipHistory.Reset();
    m_managed=false; m_mipAllocations.clear();
    ++m_generation;
    m_decode = {};
    m_queued = false;
    m_queuedProvider = nullptr;
    m_decodeFailed = false;
    m_mipLevels = m_generateMips ? FullMipCount(m_width, m_height):1;
    if(m_generateMips) GenerateMipChain(m_pixels, m_width, m_height, m_format, m_srgb);
    m_graphicsTexture.reset();
    m_preparedProvider = nullptr;
    return true;
}

void Texture::QueuePrepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || m_decodeFailed ||
        (!m_forceFull && m_graphicsTexture && m_graphicsTexture->GetNativeHandle() &&
            m_preparedProvider == graphicsProvider))
        return;
    if (m_queued)
    {
        if (m_queuedProvider == graphicsProvider) return;
        ++m_generation; // Retire the publication requested for the old provider.
    }
    if (m_pixels.empty() && !m_decode.valid())
    {
        const auto path = m_filePath;
        const bool srgb = m_srgb;
        m_decode = Engine::Rendering::RenderJobs::Get().Submit([path, srgb] {
            auto decoded = std::shared_ptr<Texture>(new Texture(path, srgb));
            return decoded->Load() ? decoded : std::shared_ptr<Texture>{};
        });
    }
    m_queued = true;
    m_queuedProvider = graphicsProvider;
    std::lock_guard lock(s_publicationMutex);
    s_publications.emplace_back(weak_from_this(), m_generation);
}

void Texture::PublishQueued(IGraphicsProvider* graphicsProvider, size_t maxUploads)
{
    StreamFrame(graphicsProvider);
    if (!graphicsProvider || !maxUploads)
        return;
    size_t pending = 0;
    {
        std::lock_guard lock(s_publicationMutex);
        pending = s_publications.size();
    }
    for (size_t checked = 0, attempted = 0;
        checked < pending && attempted < maxUploads; ++checked)
    {
        std::pair<std::weak_ptr<Texture>, uint64_t> entry;
        {
            std::lock_guard lock(s_publicationMutex);
            if (s_publications.empty()) break;
            entry = s_publications.front();
            s_publications.pop_front();
        }
        auto texture = entry.first.lock();
        if (!texture || entry.second != texture->m_generation || !texture->m_queued)
            continue;
        if (texture->m_queuedProvider != graphicsProvider)
        {
            std::lock_guard lock(s_publicationMutex);
            s_publications.push_back(std::move(entry));
            continue;
        }
        if (texture->m_decode.valid())
        {
            if (texture->m_decode.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
            {
                std::lock_guard lock(s_publicationMutex);
                s_publications.push_back(std::move(entry));
                continue;
            }
            try
            {
                auto decoded = texture->m_decode.get();
                if (decoded)
                {
                    texture->m_width = decoded->m_width;
                    texture->m_height = decoded->m_height;
                    texture->m_mipLevels = decoded->m_mipLevels;
                    texture->m_format = decoded->m_format;
                    texture->m_pixels = std::move(decoded->m_pixels);
                }
                else texture->m_decodeFailed = true;
            }
            catch (...) { texture->m_decodeFailed = true; }
        }
        texture->m_queued = false;
        texture->m_queuedProvider = nullptr;
        if (!texture->m_decodeFailed)
        {
            ++attempted;
            texture->PrepareImmediate(graphicsProvider);
        }
    }
}

bool Texture::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider) return false;
    if (!m_forceFull && m_graphicsTexture && m_graphicsTexture->GetNativeHandle() &&
        m_preparedProvider == graphicsProvider) return true;
    QueuePrepare(graphicsProvider);
    return false;
}

void Texture::ConfigureStreaming(const Engine::Rendering::TextureStreamingSettings& settings)
{
    // Graphics-owner API. A shrinking budget stops admission immediately;
    // frame-owned old images retire naturally without waiting on the GPU.
    const bool enabling=!StreamingSettings().budgetBytes && settings.budgetBytes;
    StreamingSettings() = settings;
    StreamingSettings().maxDecodes = std::clamp(settings.maxDecodes,1u,8u);
    if(!settings.budgetBytes)
    {
        std::lock_guard lock(s_textureRegistryMutex);
        for(const auto& weak:s_textureRegistry) if(auto t=weak.lock();t && t->m_managed)
        {
            t->m_managed=false;t->m_forceFull=true;
            t->m_pendingTexture.reset();t->m_fallbackTexture.reset();
            t->m_streamDecode={};t->m_streamDecoded.reset();t->m_decodeFailed=false;
        }
    }
    else if(enabling)
    {
        std::vector<std::pair<std::shared_ptr<Texture>,IGraphicsProvider*>> reload;
        {
            std::lock_guard lock(s_textureRegistryMutex);
            for(const auto& weak:s_textureRegistry) if(auto t=weak.lock();t && t->IsStreamable() && t->m_preparedProvider)
                reload.emplace_back(t,t->m_preparedProvider);
        }
        for(auto& entry:reload) {entry.first->InvalidateCache();entry.first->QueuePrepare(entry.second);}
    }
}
Texture::StreamingSnapshot Texture::GetStreamingSnapshot()
{ std::lock_guard lock(s_streamStatsMutex); return s_streamSnapshot; }
std::vector<Texture::StreamingTextureInfo> Texture::GetStreamingInventory()
{
    std::vector<StreamingTextureInfo> result;
    std::lock_guard lock(s_textureRegistryMutex);
    for(const auto& weak:s_textureRegistry) if(auto t=weak.lock())
        result.push_back({t->m_filePath+(t->m_srgb?":srgb":":linear"),t->m_width,t->m_height,
            t->m_mipLevels,t->m_residentFirstMip,t->m_demandFirstMip,t->m_managed,t->m_queued,t->m_decodeFailed});
    return result;
}
bool Texture::IsStreamable() const
{
    if (!m_generateMips || m_mipLevels <= 1 || m_format == GraphicsTextureFormat::Rgba32Float ||
        m_retainPixelsForCpu)
        return false;
    // Any level down to one block may become a streamed tail's top level,
    // which D3D requires to be a whole number of blocks.
    for (uint32_t w = m_width, h = m_height; std::min(w, h) >= 4; w /= 2, h /= 2)
        if (!Engine::Graphics::GraphicsTextureIsValidBase(m_format, w, h)) return false;
    return true;
}
void Texture::RequestMipFootprint(float pixelsPerUvRepeat)
{
    if (!std::isfinite(pixelsPerUvRepeat)) pixelsPerUvRepeat = std::numeric_limits<float>::max();
    float previous=m_projectedPixels.load(std::memory_order_relaxed);
    while(previous<pixelsPerUvRepeat && !m_projectedPixels.compare_exchange_weak(previous,pixelsPerUvRepeat,std::memory_order_relaxed)) {}
}
void Texture::RequestGraphicsFootprint(const IGraphicsTexture* resource, float pixels)
{
    if (!resource) return;
    std::shared_ptr<Texture> texture;
    {
        std::lock_guard lock(s_graphicsTextureOwnersMutex);
        const auto found=s_graphicsTextureOwners.find(resource);
        if(found!=s_graphicsTextureOwners.end()) texture=found->second.texture.lock();
    }
    if(texture) texture->RequestMipFootprint(pixels);
}
bool Texture::UploadMipTail(IGraphicsProvider* provider, uint32_t firstMip, bool replacement)
{
    using Engine::Rendering::TextureResidency;
    auto* factory = provider->GetTextureFactory();
    if (!factory || firstMip >= m_mipLevels) return false;
    uint32_t w=m_width,h=m_height;
    size_t offset=0;
    for (uint32_t mip=0;mip<firstMip;++mip)
    {
        offset += size_t(Engine::Graphics::GraphicsTextureMipBytes(m_format,w,h));
        w=std::max(1u,w/2); h=std::max(1u,h/2);
    }
    if (!Engine::Graphics::GraphicsTextureIsValidBase(m_format,w,h)) return false;
    const auto bytes=Engine::Graphics::GraphicsTextureChainBytes(m_format,w,h,m_mipLevels-firstMip);
    const auto allocation=factory->EstimateAllocationBytes(w,h,m_mipLevels-firstMip,m_format,m_srgb);
    const auto& settings=StreamingSettings();
    auto& uploadThisFrame=s_providerStreamingFrames[provider].uploaded;
    const uint64_t allocated=TextureResidency::Snapshot(provider).allocatedBytes;
    if (!allocation || allocated > settings.budgetBytes || allocation > settings.budgetBytes-allocated ||
        uploadThisFrame > settings.uploadBytesPerFrame || bytes > settings.uploadBytesPerFrame-uploadThisFrame)
    {
        m_budgetDenied=true;
        return false;
    }
    const auto& pixels=m_streamDecoded ? m_streamDecoded->m_pixels:m_pixels;
    if (offset>pixels.size() || bytes>pixels.size()-offset) return false;
    std::shared_ptr<IGraphicsTexture> resource;
    try { resource=factory->CreateTexture2D(w,h,pixels.data()+offset,m_mipLevels-firstMip,m_format,m_srgb); }
    catch (const std::exception& error)
    {
        OutputDebugStringA(("[Texture streaming] "+m_filePath+": "+error.what()+"\n").c_str());
        return false;
    }
    if (!resource || !resource->GetNativeHandle()) return false;
    m_budgetDenied=false;
    {std::lock_guard lock(s_graphicsTextureOwnersMutex);s_graphicsTextureOwners[resource.get()]={resource,weak_from_this()};}
    TextureResidency::Record(resource,bytes,Engine::Graphics::GraphicsTextureChainBytes(m_format,m_width,m_height,m_mipLevels),
        firstMip,provider,allocation);
    uploadThisFrame+=bytes; s_streamUploads+=bytes;
    if (replacement) { m_pendingTexture=std::move(resource); m_pendingFirstMip=firstMip; }
    else
    {
        m_pendingTexture.reset(); m_mipAllocations.clear(); m_mipHistory.Reset();
        m_graphicsTexture=resource; m_fallbackTexture=std::move(resource);
        m_fallbackFirstMip=firstMip;
        m_residentFirstMip=firstMip; m_demandFirstMip=firstMip;
        m_preparedProvider=provider; m_managed=true;
    }
    m_streamDecoded.reset();
    if (!m_retainPixelsForCpu) std::vector<uint8_t>().swap(m_pixels);
    return true;
}

void Texture::StreamFrame(IGraphicsProvider* provider)
{
    if (!provider || !provider->GetTextureFactory()) return;
    std::lock_guard statsLock(s_streamStatsMutex);
    auto* factory=provider->GetTextureFactory();
    factory->PollUploads();
    auto& providerFrame=s_providerStreamingFrames[provider];
    const auto* contextFactory=provider->GetContextFactory();
    const uint64_t serial=contextFactory ? contextFactory->GetPreparedFrameSerial():0;
    if(serial && providerFrame.serial==serial) return;
    providerFrame.serial=serial;providerFrame.uploaded=0;++providerFrame.frame;
    auto& uploadThisFrame=providerFrame.uploaded;
    const auto settings=StreamingSettings();
    s_streamSnapshot={}; s_streamSnapshot.budgetBytes=settings.budgetBytes;
    s_streamSnapshot.frame=providerFrame.frame;
    if (!settings.budgetBytes) return;
    {
        std::lock_guard lock(s_graphicsTextureOwnersMutex);
        for(auto it=s_graphicsTextureOwners.begin();it!=s_graphicsTextureOwners.end();)
            if(it->second.resource.expired()) it=s_graphicsTextureOwners.erase(it); else ++it;
    }
    std::vector<std::shared_ptr<Texture>> textures;
    {
        std::lock_guard lock(s_textureRegistryMutex);
        for (auto it=s_textureRegistry.begin();it!=s_textureRegistry.end();)
            if (auto texture=it->lock()) { textures.push_back(std::move(texture)); ++it; }
            else it=s_textureRegistry.erase(it);
    }
    struct Candidate { std::shared_ptr<Texture> texture; float pixels; uint32_t target; };
    std::vector<Candidate> candidates;
    uint32_t decodes=0;
    for (auto& texture:textures)
    {
        auto& t=*texture;
        if(t.m_budgetDenied) ++s_streamSnapshot.blocked;
        if (t.m_streamDecode.valid() || t.m_streamDecoded) ++decodes;
        if(!t.m_managed && t.m_preparedProvider==provider && t.m_graphicsTexture)
        {
            uint32_t w=t.m_width,h=t.m_height;
            for(uint32_t mip=0;mip<t.m_residentFirstMip;++mip) {w=std::max(1u,w/2);h=std::max(1u,h/2);}
            const auto allocation=t.m_graphicsTexture->GetAllocationSizeBytes();
            s_streamSnapshot.pinnedBytes+=allocation ? allocation:factory->EstimateAllocationBytes(w,h,t.m_mipLevels-t.m_residentFirstMip,t.m_format,t.m_srgb);
        }
        if (!t.m_managed || t.m_preparedProvider!=provider) continue;
        if (t.m_pendingTexture && t.m_pendingTexture->IsUploadComplete())
        {
            t.m_graphicsTexture=std::move(t.m_pendingTexture);
            t.m_residentFirstMip=t.m_pendingFirstMip;
        }
        if (t.m_streamDecode.valid() && t.m_streamDecode.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        {
            try { t.m_streamDecoded=t.m_streamDecode.get(); }
            catch (...) { t.m_streamDecoded.reset(); }
            if (!t.m_streamDecoded) t.m_decodeFailed=true;
        }
        if (t.m_mipAllocations.size()!=t.m_mipLevels)
        {
            t.m_mipAllocations.clear(); uint32_t w=t.m_width,h=t.m_height;
            for (uint32_t mip=0;mip<t.m_mipLevels;++mip)
            {
                t.m_mipAllocations.push_back(factory->EstimateAllocationBytes(w,h,t.m_mipLevels-mip,t.m_format,t.m_srgb));
                w=std::max(1u,w/2); h=std::max(1u,h/2);
            }
        }
        const float pixels=t.m_projectedPixels.exchange(0.f,std::memory_order_relaxed);
        t.m_demandFirstMip=Engine::Rendering::TextureDemandMip(t.m_width,t.m_height,t.m_mipLevels,pixels,settings.prefetchMips);
        uint32_t capMip=0,w=t.m_width,h=t.m_height;
        const uint32_t cap=ResidentTextureDimensionCap();
        while (cap && std::max(w,h)>cap && capMip+1<t.m_mipLevels)
        { ++capMip; w=std::max(1u,w/2); h=std::max(1u,h/2); }
        t.m_demandFirstMip=std::max(t.m_demandFirstMip,capMip);
        t.m_demandFirstMip=std::min(t.m_demandFirstMip,t.m_fallbackFirstMip);
        t.m_targetFirstMip=t.m_mipHistory.Update(providerFrame.frame,t.m_demandFirstMip,settings.holdFrames);
        w=t.m_width; h=t.m_height;
        for(uint32_t mip=0;mip<t.m_demandFirstMip;++mip) {w=std::max(1u,w/2);h=std::max(1u,h/2);}
        s_streamSnapshot.requestedBytes+=Engine::Graphics::GraphicsTextureChainBytes(t.m_format,w,h,t.m_mipLevels-t.m_demandFirstMip);
        candidates.push_back({texture,pixels,t.m_targetFirstMip});
    }
    // Stable visual priority. Every texture has an already-admitted fallback;
    // pressure may override temporal holds but never removes that fallback.
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {return a.pixels>b.pixels;});
    uint64_t planned=0;
    for (const auto& candidate:candidates)
    {
        const auto& t=*candidate.texture;
        planned+=t.m_mipAllocations[t.m_fallbackFirstMip];
        if(candidate.target<t.m_fallbackFirstMip) planned+=t.m_mipAllocations[candidate.target];
    }
    const uint64_t planningBudget=settings.budgetBytes>s_streamSnapshot.pinnedBytes ? settings.budgetBytes-s_streamSnapshot.pinnedBytes:0;
    for(auto it=candidates.rbegin();it!=candidates.rend() && planned>planningBudget;++it)
    {
        auto& t=*it->texture;
        while(it->target<t.m_fallbackFirstMip && planned>planningBudget)
        {
            planned-=t.m_mipAllocations[it->target++];
            if(it->target<t.m_fallbackFirstMip) planned+=t.m_mipAllocations[it->target];
        }
    }
    for(auto& candidate:candidates)
    {
        auto& t=*candidate.texture;
        if(candidate.target>t.m_residentFirstMip && !t.m_pendingTexture)
        {
            t.m_graphicsTexture=t.m_fallbackTexture;
            t.m_residentFirstMip=t.m_fallbackFirstMip; ++s_streamEvictions;
        }
        if(candidate.target<t.m_residentFirstMip && !t.m_pendingTexture)
        {
            const uint64_t allocated=Engine::Rendering::TextureResidency::Snapshot(provider).allocatedBytes;
            uint32_t admitted=candidate.target;
            while(admitted<t.m_residentFirstMip)
            {
                uint32_t w=t.m_width,h=t.m_height;
                for(uint32_t mip=0;mip<admitted;++mip) {w=std::max(1u,w/2);h=std::max(1u,h/2);}
                const auto bytes=Engine::Graphics::GraphicsTextureChainBytes(t.m_format,w,h,t.m_mipLevels-admitted);
                if(allocated<=settings.budgetBytes && t.m_mipAllocations[admitted]<=settings.budgetBytes-allocated &&
                    uploadThisFrame<=settings.uploadBytesPerFrame && bytes<=settings.uploadBytesPerFrame-uploadThisFrame) break;
                ++admitted;
            }
            const bool fits=admitted<t.m_residentFirstMip;
            if (!fits)
            {
                ++s_streamSnapshot.blocked;
                uint32_t w=t.m_width,h=t.m_height;
                const uint32_t next=t.m_residentFirstMip ? t.m_residentFirstMip-1:0;
                for(uint32_t mip=0;mip<next;++mip) {w=std::max(1u,w/2);h=std::max(1u,h/2);}
                const uint64_t nextBytes=Engine::Graphics::GraphicsTextureChainBytes(t.m_format,w,h,t.m_mipLevels-next);
                // An upload cap is not memory pressure. Keep the best attainable
                // resident tail instead of repeatedly evicting/reloading it.
                if(t.m_graphicsTexture!=t.m_fallbackTexture && !uploadThisFrame && nextBytes<=settings.uploadBytesPerFrame)
                { t.m_graphicsTexture=t.m_fallbackTexture; t.m_residentFirstMip=t.m_fallbackFirstMip; ++s_streamEvictions; }
            }
            else if(t.m_streamDecoded) t.UploadMipTail(provider,admitted,true);
            else if(!t.m_streamDecode.valid() && !t.m_decodeFailed && decodes<settings.maxDecodes)
            {
                const auto path=t.m_filePath; const bool srgb=t.m_srgb;
                t.m_streamDecode=Engine::Rendering::RenderJobs::Get().Submit([path,srgb] {
                    auto decoded=std::shared_ptr<Texture>(new Texture(path,srgb));
                    return decoded->Load() ? decoded:std::shared_ptr<Texture>{};
                });
                ++decodes;
            }
        }
        else if (!t.m_streamDecode.valid() && t.m_streamDecoded)
        { t.m_streamDecoded.reset(); if(decodes) --decodes; }
        s_streamSnapshot.unmet+=t.m_residentFirstMip>t.m_demandFirstMip;
        if(t.m_residentFirstMip>t.m_demandFirstMip && candidate.target>=t.m_residentFirstMip)
            ++s_streamSnapshot.blocked;
        ++s_streamSnapshot.demandMips[std::min(31u,t.m_demandFirstMip)];
        ++s_streamSnapshot.residentMips[std::min(31u,t.m_residentFirstMip)];
    }
    const auto residency=Engine::Rendering::TextureResidency::Snapshot(provider);
    s_streamSnapshot.allocatedBytes=residency.allocatedBytes;
    s_streamSnapshot.stagingBytes=factory->GetPendingUploadBytes();
    s_streamSnapshot.uploadBytes=s_streamUploads;
    s_streamSnapshot.evictions=s_streamEvictions;
    uint64_t owned=0;
    for(const auto& candidate:candidates)
    {
        const auto& t=*candidate.texture;
        owned+=t.m_mipAllocations[t.m_fallbackFirstMip];
        if(t.m_graphicsTexture!=t.m_fallbackTexture) owned+=t.m_mipAllocations[t.m_residentFirstMip];
        if(t.m_pendingTexture) owned+=t.m_mipAllocations[t.m_pendingFirstMip];
    }
    owned+=s_streamSnapshot.pinnedBytes;
    s_streamSnapshot.retiredBytes=residency.allocatedBytes>owned ? residency.allocatedBytes-owned:0;
}

bool Texture::PrepareImmediate(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider)
        return false;
    if (!m_forceFull && m_graphicsTexture && m_graphicsTexture->GetNativeHandle() &&
        m_preparedProvider == graphicsProvider)
        return true;
    if (!Load())
        return false;
    Engine::Graphics::IGraphicsTextureFactory* factory = graphicsProvider->GetTextureFactory();
    if (!factory)
        return false;
    if (Engine::Graphics::GraphicsTextureIsBlockCompressed(m_format) &&
        !factory->SupportsBlockCompression())
    {
        OutputDebugStringA(("[Texture] Device cannot sample BC textures: " + m_filePath + "\n").c_str());
        m_decodeFailed = true;
        return false;
    }
    if (StreamingSettings().budgetBytes && IsStreamable())
    {
        uint32_t firstMip=0,w=m_width,h=m_height;
        const auto cap=ResidentTextureDimensionCap();
        const uint32_t fallbackDimension=cap ? std::min(64u,cap):64u;
        while(std::max(w,h)>fallbackDimension && firstMip+1<m_mipLevels &&
            Engine::Graphics::GraphicsTextureIsValidBase(m_format,std::max(1u,w/2),std::max(1u,h/2)))
        {++firstMip;w=std::max(1u,w/2);h=std::max(1u,h/2);}
        return UploadMipTail(graphicsProvider, firstMip, false);
    }
    try
    {
        uint32_t uploadWidth = m_width;
        uint32_t uploadHeight = m_height;
        uint32_t uploadMipLevels = m_mipLevels;
        size_t uploadOffset = 0;
        uint32_t dimensionCap = ResidentTextureDimensionCap();
        // Preserve the existing panorama ceiling, including when the global
        // limit is disabled or larger. Source metadata/pixels stay unchanged.
        if (m_format == GraphicsTextureFormat::Rgba32Float)
            dimensionCap = dimensionCap ? std::min(dimensionCap, 2048u) : 2048u;
        while (dimensionCap && std::max(uploadWidth, uploadHeight) > dimensionCap &&
               uploadMipLevels > 1 &&
               Engine::Graphics::GraphicsTextureIsValidBase(m_format,
                   std::max(1u, uploadWidth / 2), std::max(1u, uploadHeight / 2)))
        {
            uploadOffset += static_cast<size_t>(
                Engine::Graphics::GraphicsTextureMipBytes(m_format, uploadWidth, uploadHeight));
            uploadWidth = std::max(1u, uploadWidth / 2);
            uploadHeight = std::max(1u, uploadHeight / 2);
            --uploadMipLevels;
        }
        auto resource =
            factory->CreateTexture2D(
                uploadWidth, uploadHeight, m_pixels.data() + uploadOffset,
                uploadMipLevels, m_format, m_srgb);
        if(!resource || !resource->GetNativeHandle()) return false;
        m_graphicsTexture=std::move(resource);
        m_forceFull=false;
        Engine::Rendering::TextureResidency::Record(m_graphicsTexture,
            Engine::Graphics::GraphicsTextureChainBytes(m_format, uploadWidth, uploadHeight,
                uploadMipLevels),
            Engine::Graphics::GraphicsTextureChainBytes(m_format, m_width, m_height,
                m_mipLevels), m_mipLevels - uploadMipLevels,
            graphicsProvider, factory->EstimateAllocationBytes(uploadWidth,uploadHeight,uploadMipLevels,m_format,m_srgb));
        if(m_graphicsTexture) m_residentFirstMip=m_mipLevels-uploadMipLevels;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(("[Texture] GPU upload failed for " + m_filePath +
            ": " + error.what() + "\n").c_str());
        return false;
    }
    m_preparedProvider = m_graphicsTexture ? graphicsProvider : nullptr;
    // Ordinary material textures remain on the GPU. Editors can decode the
    // source again through GetPixels() when they actually need CPU pixels.
    if (m_graphicsTexture && m_format != GraphicsTextureFormat::Rgba32Float &&
        !m_retainPixelsForCpu)
        std::vector<uint8_t>().swap(m_pixels);
    return static_cast<bool>(m_graphicsTexture);
}
}
