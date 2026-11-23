#include "Core/Components/Terrain/TerrainManager.h"

#include <algorithm>
#include <cmath>

// CPU twin of Object.hlsl TerrainPathHash / TerrainPathNoise / TerrainPathWeight
// (without the pixel footprint): keep the two in step.
namespace Engine::Components
{
namespace
{
uint32_t PathHash(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x9e3779b1u ^ static_cast<uint32_t>(z) * 0x85ebca77u ^
        seed * 0xc2b2ae3du;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

float PathNoise(float x, float z, float period, uint32_t seed, float& gradientX, float& gradientZ)
{
    const float px = x / period, pz = z / period;
    const float fx = std::floor(px), fz = std::floor(pz);
    const int32_t ix = static_cast<int32_t>(fx), iz = static_cast<int32_t>(fz);
    const float tx = px - fx, tz = pz - fz;
    const float sx = tx * tx * (3.f - 2.f * tx), sz = tz * tz * (3.f - 2.f * tz);
    const float dx = 6.f * tx * (1.f - tx), dz = 6.f * tz * (1.f - tz);
    const auto corner = [&](int ox, int oz) {
        return (PathHash(ix + ox, iz + oz, seed) & 0xffffffu) / 16777215.f;
    };
    const float c00 = corner(0, 0), c10 = corner(1, 0), c01 = corner(0, 1), c11 = corner(1, 1);
    const float bottom = c00 + (c10 - c00) * sx, top = c01 + (c11 - c01) * sx;
    gradientX = ((c10 - c00) * (1.f - sz) + (c11 - c01) * sz) * dx / period;
    gradientZ = (top - bottom) * dz / period;
    return bottom + (top - bottom) * sz;
}

float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
}

float TerrainManager::ContourPathShare(float worldX, float worldZ, float height) const
{
    const auto& p = contourPaths;
    if (!p.enabled) return 0.f;
    const float band = SmoothStep(p.riseStart, std::max(p.riseEnd, p.riseStart + .01f), height) *
        (1.f - SmoothStep(p.fallStart, std::max(p.fallEnd, p.fallStart + .01f), height));
    if (band <= 0.f) return 0.f;
    float ax = 0.f, az = 0.f, bx = 0.f, bz = 0.f;
    const float value = p.weightA * PathNoise(worldX, worldZ, p.periodA, p.seedA, ax, az) +
        p.weightB * PathNoise(worldX, worldZ, p.periodB, p.seedB, bx, bz);
    const float gx = p.weightA * ax + p.weightB * bx, gz = p.weightA * az + p.weightB * bz;
    const float metres = std::abs(value - p.contour) /
        std::max(std::sqrt(gx * gx + gz * gz), std::max(p.gradientFloor, 1.e-6f));
    const float track = 1.f - SmoothStep(p.core, std::max(p.edge, p.core + .01f), metres);
    return track * band * std::clamp(p.strength, 0.f, 1.f);
}
}
