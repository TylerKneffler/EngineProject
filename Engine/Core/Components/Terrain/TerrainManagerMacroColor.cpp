#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Scene/Object.h"

#include <algorithm>
#include <cmath>

// Macro color map: a scene script composes one albedo per map sample (16 m on
// the landscape demo) on a worker; this uploads it as an sRGB texture with a
// full mip chain and hands terrain draws its world mapping (texture slot 5,
// ObjectGPUData::terrainMacroMap). Polled from draw preparation, so edits to
// the script's settings rebuild it in the editor as well as in play.
namespace Engine::Components
{
namespace
{
uint8_t EncodeSrgb(float linear)
{
    linear = std::clamp(linear, 0.f, 1.f);
    const float srgb = linear <= .0031308f
        ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
    return static_cast<uint8_t>(std::lround(srgb * 255.f));
}

// How often draw preparation polls the source's revision.
constexpr auto PollInterval = std::chrono::milliseconds(100);
}

TerrainManager::MacroColorImage TerrainManager::EncodeMacroColorImage(int width,
    int height, float originX, float originZ, float spacing,
    const std::vector<glm::vec4>& linear)
{
    MacroColorImage image;
    if (width <= 0 || height <= 0 || linear.size() != size_t(width) * height)
        return image;
    image.width = width; image.height = height;
    image.originX = originX; image.originZ = originZ; image.spacing = spacing;
    // Box-filtered mips in linear space, so distant hills keep their mean
    // color instead of aliasing between forest and meadow texels.
    std::vector<glm::vec4> level = linear;
    int levelWidth = width, levelHeight = height;
    while (true)
    {
        for (const glm::vec4& value : level)
        {
            image.mips.push_back(EncodeSrgb(value.r));
            image.mips.push_back(EncodeSrgb(value.g));
            image.mips.push_back(EncodeSrgb(value.b));
            image.mips.push_back(static_cast<uint8_t>(
                std::lround(std::clamp(value.a, 0.f, 1.f) * 255.f)));
        }
        ++image.mipLevels;
        if (levelWidth == 1 && levelHeight == 1) break;
        const int nextWidth = std::max(1, levelWidth / 2);
        const int nextHeight = std::max(1, levelHeight / 2);
        std::vector<glm::vec4> next(size_t(nextWidth) * nextHeight);
        for (int y = 0; y < nextHeight; ++y)
            for (int x = 0; x < nextWidth; ++x)
            {
                glm::vec4 sum(0.f);
                int count = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = std::min(x * 2 + dx, levelWidth - 1);
                        const int sy = std::min(y * 2 + dy, levelHeight - 1);
                        sum += level[size_t(sy) * levelWidth + sx];
                        ++count;
                    }
                next[size_t(y) * nextWidth + x] = sum / float(count);
            }
        level = std::move(next);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
    }
    return image;
}

void TerrainManager::SetMacroColorSource(MacroColorSource source)
{
    if (m_macroJob.valid()) m_macroJob.wait();
    m_macroJob = {};
    m_macroSource = std::move(source);
    m_macroRequested = 0;
    m_macroChecked = {};
    if (!m_macroSource.revision)
    {
        m_macroTexture.reset();
        m_macroImage = {};
    }
}

TerrainManager::MacroColorBinding TerrainManager::GetMacroColorMap(
    IGraphicsProvider* provider)
{
    MacroColorBinding result;
    if (!macroColorMap || !provider || !Owner || !m_macroSource.revision ||
        !m_macroSource.makeBuild)
        return result;
    if (provider != m_macroProvider)
    {
        m_macroTexture.reset();
        m_macroProvider = provider;
        m_macroRequested = 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_macroChecked >= PollInterval)
    {
        m_macroChecked = now;
        if (m_macroJob.valid() &&
            m_macroJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            MacroColorImage image = m_macroJob.get();
            if (image.mipLevels && !image.mips.empty())
                if (auto* factory = provider->GetTextureFactory())
                    if (auto texture = factory->CreateTexture2D(uint32_t(image.width),
                            uint32_t(image.height), image.mips.data(), image.mipLevels,
                            Engine::Graphics::GraphicsTextureFormat::Rgba8, true))
                    {
                        m_macroTexture = std::move(texture);
                        image.mips.clear();
                        image.mips.shrink_to_fit();
                        m_macroImage = std::move(image);
                    }
        }
        // A new revision while a build runs is picked up when it finishes.
        if (!m_macroJob.valid())
            if (const uint64_t revision = m_macroSource.revision();
                revision && revision != m_macroRequested)
                if (auto build = m_macroSource.makeBuild())
                {
                    m_macroRequested = revision;
                    m_macroJob = std::async(std::launch::async, std::move(build));
                }
    }
    if (!m_macroTexture || m_macroImage.width <= 0 || m_macroImage.height <= 0)
        return result;
    // Texel centres sit on the samples, so the texture corner is half a
    // spacing before sample 0.
    const glm::dvec3 origin = Owner->transform.GetWorldPositionD();
    const float spacing = m_macroImage.spacing;
    result.texture = m_macroTexture.get();
    result.mapping = glm::vec4(
        float(origin.x) + m_macroImage.originX - .5f * spacing,
        float(origin.z) + m_macroImage.originZ - .5f * spacing,
        1.f / (spacing * float(m_macroImage.width)),
        1.f / (spacing * float(m_macroImage.height)));
    return result;
}
}
