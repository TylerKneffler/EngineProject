#include "AssetPreviewCache.h"

#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace Engine::Editor
{
namespace
{
constexpr uint32_t PreviewSize = 128;
constexpr float Pi = 3.14159265358979323846f;

std::string Extension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

float SrgbToLinear(uint8_t value)
{
    const float color = value / 255.f;
    return color <= 0.04045f
        ? color / 12.92f
        : std::pow((color + 0.055f) / 1.055f, 2.4f);
}

uint8_t LinearToSrgb(float value)
{
    value = std::clamp(value, 0.f, 1.f);
    const float encoded = value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(encoded * 255.f + 0.5f, 0.f, 255.f));
}

glm::vec3 ToneMap(glm::vec3 color)
{
    color = glm::max(color, glm::vec3(0.f));
    return color / (glm::vec3(1.f) + color);
}

void StorePixel(std::vector<uint8_t>& pixels, uint32_t x, uint32_t y,
    glm::vec3 color, uint8_t alpha = 255)
{
    color = ToneMap(color);
    const size_t index = (static_cast<size_t>(y) * PreviewSize + x) * 4;
    pixels[index] = LinearToSrgb(color.r);
    pixels[index + 1] = LinearToSrgb(color.g);
    pixels[index + 2] = LinearToSrgb(color.b);
    pixels[index + 3] = alpha;
}

glm::vec3 SampleEnvironment(const Engine::Components::Texture& texture,
    glm::vec3 direction)
{
    direction = glm::normalize(direction);
    float u = 0.5f + std::atan2(direction.z, direction.x) / (2.f * Pi);
    const float v = std::acos(std::clamp(direction.y, -1.f, 1.f)) / Pi;
    u -= std::floor(u);
    const uint32_t x = std::min(texture.GetWidth() - 1,
        static_cast<uint32_t>(u * texture.GetWidth()));
    const uint32_t y = std::min(texture.GetHeight() - 1,
        static_cast<uint32_t>(v * texture.GetHeight()));
    const size_t pixel = static_cast<size_t>(y) * texture.GetWidth() + x;
    const auto& pixels = texture.GetPixels();
    if (texture.GetFormat() == Engine::Graphics::GraphicsTextureFormat::Rgba32Float)
    {
        glm::vec3 value{};
        std::memcpy(&value.r, pixels.data() + pixel * 16, sizeof(float));
        std::memcpy(&value.g, pixels.data() + pixel * 16 + 4, sizeof(float));
        std::memcpy(&value.b, pixels.data() + pixel * 16 + 8, sizeof(float));
        return glm::max(value, glm::vec3(0.f));
    }
    const uint8_t* source = pixels.data() + pixel * 4;
    if (texture.IsSrgb())
        return { SrgbToLinear(source[0]), SrgbToLinear(source[1]),
            SrgbToLinear(source[2]) };
    return glm::vec3(source[0], source[1], source[2]) / 255.f;
}

std::vector<uint8_t> MakeEnvironmentPreview(
    const Engine::Components::Texture& environment)
{
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 0);
    const glm::vec3 view(0.f, 0.f, 1.f);
    for (uint32_t y = 0; y < PreviewSize; ++y)
    for (uint32_t x = 0; x < PreviewSize; ++x)
    {
        const float nx = ((x + 0.5f) / PreviewSize) * 2.f - 1.f;
        const float ny = 1.f - ((y + 0.5f) / PreviewSize) * 2.f;
        const float radiusSquared = nx * nx + ny * ny;
        if (radiusSquared > 1.f)
            continue;
        const glm::vec3 normal(nx, ny, std::sqrt(1.f - radiusSquared));
        const glm::vec3 reflection = 2.f * glm::dot(normal, view) * normal - view;
        StorePixel(pixels, x, y, SampleEnvironment(environment, reflection));
    }
    return pixels;
}

std::vector<uint8_t> MakeMaterialPreview(const Engine::Components::Material& material)
{
    std::vector<uint8_t> pixels(PreviewSize * PreviewSize * 4, 0);
    const glm::vec3 view(0.f, 0.f, 1.f);
    const glm::vec3 key = glm::normalize(glm::vec3(-0.45f, 0.7f, 0.6f));
    const glm::vec3 rim = glm::normalize(glm::vec3(0.65f, 0.25f, 0.4f));
    const glm::vec3 base = glm::max(material.diffuseColor, glm::vec3(0.f));
    const float metallic = std::clamp(material.metallicFactor, 0.f, 1.f);
    const float roughness = std::clamp(material.roughnessFactor, 0.045f, 1.f);
    const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), base, metallic);
    for (uint32_t y = 0; y < PreviewSize; ++y)
    for (uint32_t x = 0; x < PreviewSize; ++x)
    {
        const float nx = ((x + 0.5f) / PreviewSize) * 2.f - 1.f;
        const float ny = 1.f - ((y + 0.5f) / PreviewSize) * 2.f;
        const float radiusSquared = nx * nx + ny * ny;
        if (radiusSquared > 1.f)
            continue;
        const glm::vec3 normal(nx, ny, std::sqrt(1.f - radiusSquared));
        const float keyLight = std::max(glm::dot(normal, key), 0.f);
        const float rimLight = std::max(glm::dot(normal, rim), 0.f);
        const glm::vec3 halfVector = glm::normalize(key + view);
        const float specularPower = glm::mix(192.f, 3.f, roughness);
        const float specular = std::pow(std::max(glm::dot(normal, halfVector), 0.f),
            specularPower) * glm::mix(1.25f, 0.18f, roughness);
        const glm::vec3 reflection = 2.f * glm::dot(normal, view) * normal - view;
        const float skyBlend = std::clamp(reflection.y * 0.5f + 0.5f, 0.f, 1.f);
        const glm::vec3 studioEnvironment = glm::mix(
            glm::vec3(0.035f, 0.045f, 0.07f),
            glm::vec3(0.8f, 0.9f, 1.15f), skyBlend);
        const glm::vec3 diffuse = base * (1.f - metallic) *
            (0.08f + keyLight * 0.9f + rimLight * 0.18f);
        const glm::vec3 reflected = studioEnvironment * f0 *
            glm::mix(1.f, 0.25f, roughness) * material.reflectionStrength;
        StorePixel(pixels, x, y,
            diffuse + reflected + f0 * specular + material.emissiveColor);
    }
    return pixels;
}

std::shared_ptr<Engine::Graphics::IGraphicsTexture> Upload(
    Engine::Graphics::IGraphicsProvider* provider, const std::vector<uint8_t>& pixels)
{
    if (!provider || !provider->GetTextureFactory())
        return nullptr;
    return provider->GetTextureFactory()->CreateTexture2D(
        PreviewSize, PreviewSize, pixels.data(), 1,
        Engine::Graphics::GraphicsTextureFormat::Rgba8, true);
}
}

bool AssetPreviewCache::Supports(const std::string& path)
{
    const std::string extension = Extension(path);
    static constexpr std::array<const char*, 11> supported{
        ".material", ".mat", ".hdr", ".exr", ".png", ".jpg", ".jpeg",
        ".bmp", ".dds", ".tga", ".ktx2" };
    return std::find(supported.begin(), supported.end(), extension) != supported.end();
}

bool AssetPreviewCache::IsCircularPreview(const std::string& path)
{
    const std::string extension = Extension(path);
    return extension == ".material" || extension == ".mat" ||
        extension == ".hdr" || extension == ".exr";
}

void* AssetPreviewCache::Get(const std::string& path,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || !Supports(path))
        return nullptr;
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return nullptr;
    Entry& entry = m_entries[path];
    if (entry.provider == graphicsProvider && entry.writeTime == writeTime &&
        (entry.generated || entry.source))
    {
        const auto* texture = entry.generated
            ? entry.generated.get()
            : entry.source->GetGraphicsTexture();
        return texture ? texture->GetNativeHandle() : nullptr;
    }

    entry = {};
    entry.writeTime = writeTime;
    entry.provider = graphicsProvider;
    const std::string extension = Extension(path);
    if (extension == ".material" || extension == ".mat")
    {
        Engine::Components::Material material;
        if (material.LoadFromFile(path))
            entry.generated = Upload(graphicsProvider, MakeMaterialPreview(material));
    }
    else
    {
        const bool highDynamicRange = extension == ".hdr" || extension == ".exr";
        entry.source = Engine::Components::Texture::Acquire(path, !highDynamicRange);
        if (highDynamicRange)
        {
            if (entry.source->Load())
                entry.generated = Upload(graphicsProvider,
                    MakeEnvironmentPreview(*entry.source));
        }
        else
            entry.source->Prepare(graphicsProvider);
    }
    const auto* texture = entry.generated
        ? entry.generated.get()
        : (entry.source ? entry.source->GetGraphicsTexture() : nullptr);
    return texture ? texture->GetNativeHandle() : nullptr;
}

void AssetPreviewCache::Invalidate(const std::string& path)
{
    m_entries.erase(path);
}

void AssetPreviewCache::Clear()
{
    m_entries.clear();
}
}
