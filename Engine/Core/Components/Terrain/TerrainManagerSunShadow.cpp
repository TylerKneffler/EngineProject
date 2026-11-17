#include "TerrainManager.h"
#include "Core/Scene/Object.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

// Long-range terrain sun shadow. Every texel of a map over the far height
// grid marches toward the sun and records the highest horizon angle it meets;
// visibility is the sun's elevation against that horizon, softened over a
// penumbra. Realtime cascades still provide the near, detailed shadows.
namespace Engine::Components
{
namespace
{
constexpr int TexelsPerCell = 4;
constexpr uint32_t MaxSide = 1024;
// Rebuild once the sun has moved this far (radians).
constexpr float RebuildAngle = 0.0044f;

struct HeightGrid
{
    const std::vector<float>& heights;
    int side = 0;
    float At(float x, float z) const
    {
        x = std::clamp(x, 0.f, float(side - 1));
        z = std::clamp(z, 0.f, float(side - 1));
        const int x0 = std::min(int(x), side - 2), z0 = std::min(int(z), side - 2);
        const float fx = x - x0, fz = z - z0;
        const size_t row0 = size_t(z0) * side, row1 = row0 + side;
        const float top = heights[row0 + x0] + (heights[row0 + x0 + 1] - heights[row0 + x0]) * fx;
        const float bottom = heights[row1 + x0] + (heights[row1 + x0 + 1] - heights[row1 + x0]) * fx;
        return top + (bottom - top) * fz;
    }
};

std::vector<uint8_t> MarchSunVisibility(const std::vector<float>& heights, int side,
    float spanX, float spanZ, glm::vec3 direction, float softnessDegrees,
    uint32_t textureSide)
{
    std::vector<uint8_t> pixels(size_t(textureSide) * textureSide * 4u, 255u);
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    if (side < 2 || horizontal < 1e-4f)
        return pixels;
    const HeightGrid grid{ heights, side };
    const float cells = float(side - 1);
    const float maxHeight = *std::max_element(heights.begin(), heights.end());
    const float sunAngle = std::atan2(direction.y, horizontal);
    const float softness = std::max(glm::radians(softnessDegrees), 1e-3f);
    // Steps in grid units per metre travelled toward the sun.
    const float stepX = direction.x / horizontal / spanX;
    const float stepZ = direction.z / horizontal / spanZ;
    const float texelMetres = cells * std::min(spanX, spanZ) / float(textureSide);
    const float maxDistance = cells * std::max(spanX, spanZ) * 1.5f;
    const float topAngle = sunAngle + softness * 0.5f;
    // Past this horizon slope the texel is fully shadowed.
    const float darkTan = topAngle < 1.55f ? std::tan(topAngle) : std::numeric_limits<float>::max();
    // A ray climbing at the penumbra's upper edge clears every peak beyond
    // the distance where it passes the grid's highest sample.
    const float clearTan = std::tan(std::clamp(topAngle, -1.5f, 1.5f));

    const auto marchRow = [&](uint32_t row)
    {
        const float z = (float(row) + .5f) / float(textureSide) * cells;
        for (uint32_t column = 0; column < textureSide; ++column)
        {
            const float x = (float(column) + .5f) / float(textureSide) * cells;
            const float start = grid.At(x, z) + 1.f;
            float horizonTan = -std::numeric_limits<float>::max();
            float t = texelMetres * .75f;
            while (t < maxDistance)
            {
                const float sx = x + stepX * t, sz = z + stepZ * t;
                if (sx < 0.f || sz < 0.f || sx > cells || sz > cells) break;
                horizonTan = std::max(horizonTan, (grid.At(sx, sz) - start) / t);
                if (horizonTan >= darkTan) break;
                if (start + t * clearTan > maxHeight) break;
                t += std::max(texelMetres * .5f, t * .04f);
            }
            const float horizonAngle = horizonTan > -1e30f ? std::atan(horizonTan) : -1.6f;
            const float visibility = std::clamp(
                (sunAngle - horizonAngle) / softness + .5f, 0.f, 1.f);
            const uint8_t value = uint8_t(std::lround(visibility * 255.f));
            uint8_t* texel = &pixels[(size_t(row) * textureSide + column) * 4u];
            texel[0] = texel[1] = texel[2] = value;
            texel[3] = 255u;
        }
    };
    const uint32_t threadCount = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 8u);
    std::vector<std::thread> threads;
    for (uint32_t thread = 1; thread < threadCount; ++thread)
        threads.emplace_back([&, thread] {
            for (uint32_t row = thread; row < textureSide; row += threadCount) marchRow(row);
        });
    for (uint32_t row = 0; row < textureSide; row += threadCount) marchRow(row);
    for (auto& thread : threads) thread.join();
    return pixels;
}
}

void TerrainManager::ResetSunShadow()
{
    std::lock_guard<std::mutex> lock(m_sunShadowMutex);
    m_sunShadowTexture.reset();
    m_sunShadowHeights.reset();
    m_sunShadowPending = {};
    m_sunShadowRequest = {};
}

TerrainManager::SunShadowMap TerrainManager::GetSunShadowMap(
    IGraphicsProvider* provider, const glm::vec3& directionToSun)
{
    SunShadowMap result;
    if (!longRangeShadows || !provider || !Owner || !m_farHeightsSnapshot ||
        m_farHalfExtentCells <= 0 || glm::dot(directionToSun, directionToSun) < 1e-8f)
        return result;
    const int side = m_farHalfExtentCells * 2 + 2;
    if (m_farHeightsSnapshot->size() != size_t(side) * side)
        return result;
    const glm::vec3 direction = glm::normalize(directionToSun);
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    const float softness = std::clamp(longRangeShadowSoftness, .1f, 20.f);

    std::lock_guard<std::mutex> lock(m_sunShadowMutex);
    if (provider != m_sunShadowProvider)
    {
        m_sunShadowTexture.reset();
        m_sunShadowHeights.reset();
        m_sunShadowProvider = provider;
    }
    if (m_sunShadowJob.valid() &&
        m_sunShadowJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        m_sunShadowPending = m_sunShadowJob.get();
    if (!m_sunShadowPending.pixels.empty())
    {
        if (m_sunShadowPending.heights != m_farHeightsSnapshot)
            m_sunShadowPending = {};
        else if (auto* factory = provider->GetTextureFactory())
            if (auto texture = factory->CreateTexture2D(m_sunShadowPending.side,
                    m_sunShadowPending.side, m_sunShadowPending.pixels.data(), 1,
                    Engine::Graphics::GraphicsTextureFormat::Rgba8, false))
            {
                m_sunShadowTexture = std::move(texture);
                m_sunShadowHeights = m_sunShadowPending.heights;
                m_sunShadowPending = {};
            }
    }

    const bool stale = m_sunShadowRequest.heights != m_farHeightsSnapshot ||
        m_sunShadowRequest.softness != softness ||
        glm::dot(direction, m_sunShadowRequest.direction) < std::cos(RebuildAngle);
    if (stale && !m_sunShadowJob.valid())
    {
        m_sunShadowRequest.direction = direction;
        m_sunShadowRequest.softness = softness;
        m_sunShadowRequest.heights = m_farHeightsSnapshot;
        m_sunShadowRequest.side = std::min<uint32_t>(MaxSide,
            uint32_t(side - 1) * TexelsPerCell);
        SunShadowBuild request = m_sunShadowRequest;
        m_sunShadowJob = std::async(std::launch::async,
            [request = std::move(request), side, spanX, spanZ]() mutable
            {
                request.pixels = MarchSunVisibility(*request.heights, side,
                    spanX, spanZ, request.direction, request.softness, request.side);
                return request;
            });
    }

    if (m_sunShadowTexture && m_sunShadowHeights == m_farHeightsSnapshot)
    {
        const glm::dvec3 origin = Owner->transform.GetWorldPositionD();
        const float sizeX = float(side - 1) * spanX, sizeZ = float(side - 1) * spanZ;
        result.texture = m_sunShadowTexture.get();
        result.mapping = glm::vec4(
            float(origin.x) - float(m_farHalfExtentCells) * spanX,
            float(origin.z) - float(m_farHalfExtentCells) * spanZ,
            1.f / sizeX, 1.f / sizeZ);
    }
    return result;
}
}
