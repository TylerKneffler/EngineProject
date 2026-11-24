#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Components/Terrain/TerrainStamp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>

namespace Engine::Components
{
namespace
{
float Bilinear(const std::vector<float>& grid, int resolution, const glm::vec2& uv)
{
    if (resolution < 2 || grid.size() != size_t(resolution) * resolution) return 0.f;
    if (!(std::abs(uv.x) <= 1.f && std::abs(uv.y) <= 1.f)) return 0.f;
    const float fx = (uv.x * .5f + .5f) * (resolution - 1);
    const float fz = (uv.y * .5f + .5f) * (resolution - 1);
    const int x0 = std::clamp(int(std::floor(fx)), 0, resolution - 2);
    const int z0 = std::clamp(int(std::floor(fz)), 0, resolution - 2);
    const float tx = std::clamp(fx - x0, 0.f, 1.f), tz = std::clamp(fz - z0, 0.f, 1.f);
    const auto at = [&](int x, int z) { return grid[size_t(z) * resolution + x]; };
    return (at(x0, z0) * (1.f - tx) + at(x0 + 1, z0) * tx) * (1.f - tz) +
        (at(x0, z0 + 1) * (1.f - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}
}

float TerrainBrushShape::SampleHeight(const glm::vec2& uv) const
{
    return Bilinear(heights, resolution, uv);
}

float TerrainBrushShape::SampleMask(const glm::vec2& uv) const
{
    return Bilinear(mask, resolution, uv);
}

glm::vec2 TerrainBrushShape::ToShape(float dx, float dz, float radius, float yawRadians)
{
    if (!(radius > 0.f)) return glm::vec2(2.f);
    const float c = std::cos(yawRadians), s = std::sin(yawRadians);
    // Undo a rotation by yaw about +Y (x' = c x + s z, z' = -s x + c z).
    return glm::vec2(c * dx - s * dz, s * dx + c * dz) / radius;
}

glm::vec2 TerrainBrushShape::FromShape(const glm::vec2& uv, float radius, float yawRadians)
{
    const float c = std::cos(yawRadians), s = std::sin(yawRadians);
    const glm::vec2 p = uv * radius;
    return glm::vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
}

std::shared_ptr<const TerrainBrushShape> TerrainBrushShape::FromStamp(const TerrainStamp& stamp,
    int resolution)
{
    if (stamp.levels.empty() || stamp.levels.front().size() < 3 || resolution < 8) return nullptr;
    const auto& indices = stamp.levels.front();
    // Footprint: the X/Z bounds of every point the triangles use.
    glm::vec2 low(std::numeric_limits<float>::max()), high(-std::numeric_limits<float>::max());
    for (uint32_t index : indices)
    {
        const glm::vec3& p = stamp.points[index].position;
        low = glm::min(low, glm::vec2(p.x, p.z));
        high = glm::max(high, glm::vec2(p.x, p.z));
    }
    const glm::vec2 centre = (low + high) * .5f;
    const float halfExtent = std::max(high.x - low.x, high.y - low.y) * .5f * 1.04f;
    if (!(halfExtent > 1.e-4f)) return nullptr;

    // Top-down z-buffer: the highest surface over each sample.
    const int n = resolution;
    const float lowest = -std::numeric_limits<float>::max();
    std::vector<float> top(size_t(n) * n, lowest);
    const auto toGrid = [&](const glm::vec3& p)
    {
        return glm::vec3(((p.x - centre.x) / halfExtent * .5f + .5f) * (n - 1), p.y,
            ((p.z - centre.y) / halfExtent * .5f + .5f) * (n - 1));
    };
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
    {
        const glm::vec3 a = toGrid(stamp.points[indices[t]].position);
        const glm::vec3 b = toGrid(stamp.points[indices[t + 1]].position);
        const glm::vec3 c = toGrid(stamp.points[indices[t + 2]].position);
        const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
        const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(n - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
        const int z0 = std::max(0, int(std::floor(std::min({a.z, b.z, c.z}))));
        const int z1 = std::min(n - 1, int(std::ceil(std::max({a.z, b.z, c.z}))));
        // Faces seen edge-on from above (sheer walls) cover no area: their
        // top edge still marks the samples they pass through.
        if (std::abs(area) < 1.e-6f)
        {
            for (const glm::vec3* p : {&a, &b, &c})
            {
                const int x = std::clamp(int(std::lround(p->x)), 0, n - 1);
                const int z = std::clamp(int(std::lround(p->z)), 0, n - 1);
                top[size_t(z) * n + x] = std::max(top[size_t(z) * n + x], p->y);
            }
            continue;
        }
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const float px = float(x), pz = float(z);
                const float w0 = ((b.x - px) * (c.z - pz) - (c.x - px) * (b.z - pz)) / area;
                const float w1 = ((c.x - px) * (a.z - pz) - (a.x - px) * (c.z - pz)) / area;
                const float w2 = 1.f - w0 - w1;
                constexpr float edge = -.02f;
                if (w0 < edge || w1 < edge || w2 < edge) continue;
                const float y = w0 * a.y + w1 * b.y + w2 * c.y;
                float& sample = top[size_t(z) * n + x];
                sample = std::max(sample, y);
            }
    }
    // Close pinholes between triangles: an empty sample with covered samples
    // on opposite sides takes their mean.
    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<float> filled = top;
        for (int z = 1; z < n - 1; ++z)
            for (int x = 1; x < n - 1; ++x)
            {
                if (top[size_t(z) * n + x] != lowest) continue;
                const float l = top[size_t(z) * n + x - 1], r = top[size_t(z) * n + x + 1];
                const float d = top[size_t(z - 1) * n + x], u = top[size_t(z + 1) * n + x];
                if (l != lowest && r != lowest) filled[size_t(z) * n + x] = .5f * (l + r);
                else if (d != lowest && u != lowest) filled[size_t(z) * n + x] = .5f * (d + u);
            }
        top.swap(filled);
    }

    // Base: the height the shape stands on (a low percentile of its
    // outline's ring), so the brush adds rock above the ground, not a plinth.
    std::vector<float> rim;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            if (top[size_t(z) * n + x] == lowest) continue;
            bool border = x == 0 || z == 0 || x == n - 1 || z == n - 1;
            for (int o = 0; o < 4 && !border; ++o)
            {
                const int nx = x + (o == 0) - (o == 1), nz = z + (o == 2) - (o == 3);
                border = top[size_t(nz) * n + nx] == lowest;
            }
            if (border) rim.push_back(top[size_t(z) * n + x]);
        }
    if (rim.empty()) return nullptr;
    std::nth_element(rim.begin(), rim.begin() + rim.size() / 10, rim.end());
    const float base = rim[rim.size() / 10];

    auto shape = std::make_shared<TerrainBrushShape>();
    shape->resolution = n;
    shape->halfExtent = halfExtent;
    if (const glm::vec2 facing(stamp.direction.x, stamp.direction.z); glm::length(facing) > 1.e-3f)
        shape->facing = glm::normalize(facing);
    shape->heights.assign(size_t(n) * n, 0.f);
    shape->mask.assign(size_t(n) * n, 0.f);
    // Feather: samples within a few steps of the outline fade out, so a
    // stamp's edge meets the ground without a step.
    const int feather = std::max(2, n / 40);
    std::vector<int> inside(size_t(n) * n, 0);
    for (size_t i = 0; i < top.size(); ++i) inside[i] = top[i] != lowest ? feather : 0;
    for (int step = 0; step < feather; ++step)
    {
        std::vector<int> next = inside;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
            {
                int& value = next[size_t(z) * n + x];
                if (!value) continue;
                for (int o = 0; o < 4; ++o)
                {
                    const int nx = x + (o == 0) - (o == 1), nz = z + (o == 2) - (o == 3);
                    const int neighbour = (nx < 0 || nz < 0 || nx >= n || nz >= n)
                        ? 0 : inside[size_t(nz) * n + nx];
                    value = std::min(value, neighbour + 1);
                }
            }
        inside.swap(next);
    }
    for (size_t i = 0; i < top.size(); ++i)
    {
        if (top[i] == lowest) continue;
        const float t = std::clamp(float(inside[i]) / feather, 0.f, 1.f);
        shape->mask[i] = t * t * (3.f - 2.f * t);
        shape->heights[i] = std::max(0.f, top[i] - base);
        shape->maxHeight = std::max(shape->maxHeight, shape->heights[i]);
    }
    // Outline: the farthest covered sample along each of 64 directions.
    for (int i = 0; i < 64; ++i)
    {
        const float angle = 6.2831853f * i / 64.f;
        const glm::vec2 direction(std::cos(angle), std::sin(angle));
        float reach = 0.f;
        for (float r = 0.f; r <= 1.415f; r += 1.f / n)
            if (shape->SampleMask(direction * r) > .5f) reach = r;
        shape->outline.push_back(direction * reach);
    }
    return shape;
}

std::shared_ptr<const TerrainBrushShape> TerrainBrushShape::Load(const std::string& stampPath)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TerrainBrushShape>> cache;
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(stampPath); found != cache.end()) return found->second;
    std::shared_ptr<const TerrainBrushShape> shape;
    if (const auto stamp = TerrainStamp::Load(stampPath)) shape = FromStamp(*stamp);
    cache[stampPath] = shape;
    return shape;
}
}
