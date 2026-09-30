#include "EnvironmentMap.h"

#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Memory/CacheStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace Engine::Rendering
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;

float SrgbToLinear(uint8_t value)
{
    const float v = value / 255.f;
    return v <= 0.04045f ? v / 12.92f
        : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 ReadSourcePixel(const Engine::Components::Texture& source,
    uint32_t x, uint32_t y)
{
    x = std::min(x, source.GetWidth() - 1u);
    y = std::min(y, source.GetHeight() - 1u);
    const auto& pixels = source.GetPixels();
    const size_t index = static_cast<size_t>(y) * source.GetWidth() + x;
    if (source.GetFormat() ==
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
    {
        glm::vec3 value{};
        std::memcpy(&value.x, pixels.data() + index * 16u, sizeof(float));
        std::memcpy(&value.y, pixels.data() + index * 16u + 4u, sizeof(float));
        std::memcpy(&value.z, pixels.data() + index * 16u + 8u, sizeof(float));
        return glm::max(value, glm::vec3(0.f));
    }
    const uint8_t* value = pixels.data() + index * 4u;
    if (source.IsSrgb())
        return { SrgbToLinear(value[0]), SrgbToLinear(value[1]),
            SrgbToLinear(value[2]) };
    return glm::vec3(value[0], value[1], value[2]) / 255.f;
}

glm::vec3 SamplePanorama(const Engine::Components::Texture& source,
    const glm::vec3& direction)
{
    float u = 0.5f + std::atan2(direction.z, direction.x) / kTwoPi;
    u -= std::floor(u);
    const float v = std::acos(std::clamp(direction.y, -1.f, 1.f)) / kPi;
    const float px = u * source.GetWidth() - 0.5f;
    const float py = v * source.GetHeight() - 0.5f;
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const float tx = px - std::floor(px);
    const float ty = py - std::floor(py);
    const auto wrapX = [&source](int x)
    {
        const int width = static_cast<int>(source.GetWidth());
        return static_cast<uint32_t>((x % width + width) % width);
    };
    const auto clampY = [&source](int y)
    {
        return static_cast<uint32_t>(std::clamp(y, 0,
            static_cast<int>(source.GetHeight()) - 1));
    };
    const glm::vec3 a = glm::mix(ReadSourcePixel(source, wrapX(x0), clampY(y0)),
        ReadSourcePixel(source, wrapX(x0 + 1), clampY(y0)), tx);
    const glm::vec3 b = glm::mix(ReadSourcePixel(source, wrapX(x0), clampY(y0 + 1)),
        ReadSourcePixel(source, wrapX(x0 + 1), clampY(y0 + 1)), tx);
    return glm::mix(a, b, ty);
}

// Face order/layout: +X -X +Y / -Y +Z -Z.
glm::vec3 CubeDirection(uint32_t face, float u, float v)
{
    const float x = u * 2.f - 1.f;
    const float y = 1.f - v * 2.f;
    switch (face)
    {
    case 0: return glm::normalize(glm::vec3(1.f, y, -x));
    case 1: return glm::normalize(glm::vec3(-1.f, y, x));
    case 2: return glm::normalize(glm::vec3(x, 1.f, -y));
    case 3: return glm::normalize(glm::vec3(x, -1.f, y));
    case 4: return glm::normalize(glm::vec3(x, y, 1.f));
    default:return glm::normalize(glm::vec3(-x, y, -1.f));
    }
}

void WriteAtlasPixel(std::vector<uint8_t>& pixels, size_t mipOffset,
    uint32_t faceSize, uint32_t face, uint32_t x, uint32_t y,
    const glm::vec3& color)
{
    const uint32_t atlasWidth = faceSize * 3u;
    const uint32_t atlasX = (face % 3u) * faceSize + x;
    const uint32_t atlasY = (face / 3u) * faceSize + y;
    const size_t offset = mipOffset +
        (static_cast<size_t>(atlasY) * atlasWidth + atlasX) * 16u;
    const float alpha = 1.f;
    std::memcpy(pixels.data() + offset, &color.x, sizeof(float));
    std::memcpy(pixels.data() + offset + 4u, &color.y, sizeof(float));
    std::memcpy(pixels.data() + offset + 8u, &color.z, sizeof(float));
    std::memcpy(pixels.data() + offset + 12u, &alpha, sizeof(float));
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

std::array<float, 9> ShBasis(const glm::vec3& d)
{
    return { 0.282095f, 0.488603f * d.y, 0.488603f * d.z,
        0.488603f * d.x, 1.092548f * d.x * d.y,
        1.092548f * d.y * d.z, 0.315392f * (3.f * d.z * d.z - 1.f),
        1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y) };
}

uint32_t FloorPowerOfTwo(uint32_t value)
{
    uint32_t result = 1u;
    while (result <= value / 2u)
        result *= 2u;
    return result;
}
}

std::shared_ptr<EnvironmentMap> EnvironmentMap::Acquire(
    const Engine::Components::Texture& source)
{
    if (!source.HasPixels() || !source.GetWidth() || !source.GetHeight())
        return nullptr;
    const std::string key = Engine::Memory::CacheStore::PathKey(
        source.GetFilePath(), "cubemap-v1");
    return Engine::Memory::CacheStore::Get().GetOrCreate<EnvironmentMap>(
        Engine::Memory::CacheLifetime::LongTerm, "Lighting.EnvironmentMap", key,
        [&source]()
        {
            std::shared_ptr<EnvironmentMap> result(
                new EnvironmentMap(source));
            return result->m_faceSize ? result : nullptr;
        });
}

void EnvironmentMap::Invalidate(const std::string& sourcePath)
{
    Engine::Memory::CacheStore::Get().Erase("Lighting.EnvironmentMap",
        Engine::Memory::CacheStore::PathKey(sourcePath, "cubemap-v1"));
}

EnvironmentMap::EnvironmentMap(const Engine::Components::Texture& source)
    : m_sourcePath(std::filesystem::path(source.GetFilePath())
        .lexically_normal().generic_string())
{
    Build(source);
}

bool EnvironmentMap::Build(const Engine::Components::Texture& source)
{
    if (!source.HasPixels() || !source.GetWidth() || !source.GetHeight())
        return false;

    // A panorama normally has four horizontal texels per cube-face texel.
    m_faceSize = std::clamp(FloorPowerOfTwo(source.GetWidth() / 4u), 1u, 256u);
    m_irradianceFaceSize = std::min(32u, m_faceSize);
    for (uint32_t size = m_faceSize; ; size = std::max(1u, size / 2u))
    {
        ++m_mipLevels;
        if (size == 1u) break;
    }

    size_t specularBytes = 0;
    for (uint32_t size = m_faceSize, mip = 0; mip < m_mipLevels;
        ++mip, size = std::max(1u, size / 2u))
        specularBytes += static_cast<size_t>(size * 3u) * (size * 2u) * 16u;
    m_specularPixels.resize(specularBytes);
    m_irradiancePixels.resize(static_cast<size_t>(m_irradianceFaceSize * 3u) *
        (m_irradianceFaceSize * 2u) * 16u);

    size_t mipOffset = 0;
    uint32_t size = m_faceSize;
    constexpr uint32_t specularSamples = 64u;
    for (uint32_t mip = 0; mip < m_mipLevels; ++mip)
    {
        const float roughness = static_cast<float>(mip) /
            static_cast<float>(std::max(1u, m_mipLevels - 1u));
        const float alpha = std::max(roughness * roughness, 0.001f);
        const float alpha2 = alpha * alpha;
        for (uint32_t face = 0; face < 6u; ++face)
        for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const glm::vec3 normal = CubeDirection(face,
                (x + 0.5f) / size, (y + 0.5f) / size);
            glm::vec3 radiance{};
            if (mip == 0u)
                radiance = SamplePanorama(source, normal);
            else
            {
                const glm::vec3 tangent = std::abs(normal.y) < 0.999f
                    ? glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), normal))
                    : glm::vec3(1.f, 0.f, 0.f);
                const glm::vec3 bitangent = glm::cross(normal, tangent);
                float weight = 0.f;
                for (uint32_t sample = 0; sample < specularSamples; ++sample)
                {
                    const float xiX = static_cast<float>(sample) / specularSamples;
                    const float xiY = RadicalInverseVdc(sample);
                    const float phi = kTwoPi * xiX;
                    const float cosTheta = std::sqrt(std::max(0.f,
                        (1.f - xiY) / (1.f + (alpha2 - 1.f) * xiY)));
                    const float sinTheta = std::sqrt(std::max(0.f,
                        1.f - cosTheta * cosTheta));
                    const glm::vec3 halfVector = glm::normalize(
                        tangent * (std::cos(phi) * sinTheta) +
                        bitangent * (std::sin(phi) * sinTheta) +
                        normal * cosTheta);
                    const glm::vec3 light = glm::normalize(
                        2.f * glm::dot(normal, halfVector) * halfVector - normal);
                    const float noL = std::max(glm::dot(normal, light), 0.f);
                    if (noL > 0.f)
                    {
                        radiance += SamplePanorama(source, light) * noL;
                        weight += noL;
                    }
                }
                radiance /= std::max(weight, 0.0001f);
            }
            WriteAtlasPixel(m_specularPixels, mipOffset, size,
                face, x, y, radiance);
        }
        mipOffset += static_cast<size_t>(size * 3u) * (size * 2u) * 16u;
        size = std::max(1u, size / 2u);
    }

    constexpr uint32_t diffuseSamples = 128u;
    for (uint32_t face = 0; face < 6u; ++face)
    for (uint32_t y = 0; y < m_irradianceFaceSize; ++y)
    for (uint32_t x = 0; x < m_irradianceFaceSize; ++x)
    {
        const glm::vec3 normal = CubeDirection(face,
            (x + 0.5f) / m_irradianceFaceSize,
            (y + 0.5f) / m_irradianceFaceSize);
        const glm::vec3 tangent = std::abs(normal.y) < 0.999f
            ? glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), normal))
            : glm::vec3(1.f, 0.f, 0.f);
        const glm::vec3 bitangent = glm::cross(normal, tangent);
        glm::vec3 irradiance{};
        for (uint32_t sample = 0; sample < diffuseSamples; ++sample)
        {
            const float xiX = static_cast<float>(sample) / diffuseSamples;
            const float xiY = RadicalInverseVdc(sample);
            const float radius = std::sqrt(xiY);
            const float phi = kTwoPi * xiX;
            const glm::vec3 direction = glm::normalize(
                tangent * (std::cos(phi) * radius) +
                bitangent * (std::sin(phi) * radius) +
                normal * std::sqrt(std::max(0.f, 1.f - xiY)));
            irradiance += SamplePanorama(source, direction);
        }
        irradiance *= kPi / static_cast<float>(diffuseSamples);
        WriteAtlasPixel(m_irradiancePixels, 0u, m_irradianceFaceSize,
            face, x, y, irradiance);
    }

    // Keep radiance SH for the compact diffuse shader path. It is generated
    // from the same cubemap directions and cached with both texture products.
    float totalWeight = 0.f;
    const uint32_t shSize = std::min(64u, m_faceSize);
    for (uint32_t face = 0; face < 6u; ++face)
    for (uint32_t y = 0; y < shSize; ++y)
    for (uint32_t x = 0; x < shSize; ++x)
    {
        const float uc = 2.f * (x + 0.5f) / shSize - 1.f;
        const float vc = 2.f * (y + 0.5f) / shSize - 1.f;
        const float weight = 1.f / std::pow(1.f + uc * uc + vc * vc, 1.5f);
        const glm::vec3 direction = CubeDirection(face,
            (x + 0.5f) / shSize, (y + 0.5f) / shSize);
        const glm::vec3 radiance = SamplePanorama(source, direction);
        const auto basis = ShBasis(direction);
        for (size_t coefficient = 0; coefficient < basis.size(); ++coefficient)
            m_radianceSH[coefficient] +=
                glm::vec4(radiance * basis[coefficient] * weight, 0.f);
        totalWeight += weight;
    }
    if (totalWeight > 0.f)
        for (glm::vec4& coefficient : m_radianceSH)
            coefficient *= 4.f * kPi / totalWeight;
    return true;
}

bool EnvironmentMap::Prepare(
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || !m_faceSize)
        return false;
    if (m_preparedProvider == graphicsProvider && m_specularTexture &&
        m_irradianceTexture)
        return true;
    auto* factory = graphicsProvider->GetTextureFactory();
    if (!factory)
        return false;
    m_specularTexture = factory->CreateTexture2D(
        m_faceSize * 3u, m_faceSize * 2u, m_specularPixels.data(),
        m_mipLevels, Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false);
    m_irradianceTexture = factory->CreateTexture2D(
        m_irradianceFaceSize * 3u, m_irradianceFaceSize * 2u,
        m_irradiancePixels.data(), 1u,
        Engine::Graphics::GraphicsTextureFormat::Rgba32Float, false);
    m_preparedProvider = m_specularTexture && m_irradianceTexture
        ? graphicsProvider : nullptr;
    return m_preparedProvider != nullptr;
}
}
