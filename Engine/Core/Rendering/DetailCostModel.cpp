#include "Core/Rendering/DetailCostModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Rendering
{
double DetailCostCurve::CostAt(float scale) const
{
    const float position = (std::log(std::max(scale, 1.e-6f)) - MinimumLog) / Step - .5f;
    if (position <= 0.f) return cost.front();
    if (position >= Buckets - 1) return cost.back();
    const int index = static_cast<int>(position);
    const double t = position - index;
    return cost[index] + (cost[index + 1] - cost[index]) * t;
}

float DetailCostCurve::LargestScaleWithin(double budget, float minimum, float maximum) const
{
    const float logMinimum = std::log(minimum), logMaximum = std::log(maximum);
    if (CostAt(maximum) <= budget) return maximum;
    // Walk down from the maximum to the first bucket that fits, then solve
    // linearly between it and the bucket above.
    float above = logMaximum;
    double aboveCost = CostAt(maximum);
    for (int index = Buckets - 1; index >= 0; --index)
    {
        const float u = LogScale(index);
        if (u >= logMaximum) continue;
        if (u <= logMinimum) break;
        if (cost[index] <= budget)
        {
            const double span = aboveCost - cost[index];
            const float t = span > 0.0 ? static_cast<float>((budget - cost[index]) / span) : 0.f;
            return std::clamp(std::exp(u + (above - u) * std::clamp(t, 0.f, 1.f)), minimum, maximum);
        }
        above = u;
        aboveCost = cost[index];
    }
    return minimum;
}

namespace
{
constexpr int GridWidth = 64, GridHeight = 36;

struct Placement
{
    float distance;     // camera to placement origin (what LOD bands use)
    float order;        // front-to-back key: nearest point of the bounds
    float x, y, radius; // screen disc, pixels
    uint32_t group;
};

int FirstBucketAbove(float u)
{
    if (u == -std::numeric_limits<float>::infinity()) return 0;
    if (u == std::numeric_limits<float>::infinity()) return DetailCostCurve::Buckets;
    const float position = (u - DetailCostCurve::MinimumLog) / DetailCostCurve::Step - .5f;
    return std::clamp(static_cast<int>(std::floor(position)) + 1, 0, DetailCostCurve::Buckets);
}
}

DetailCostCurve BuildDetailCostCurve(const DetailCostInput& input)
{
    DetailCostCurve curve;
    const glm::mat4& m = input.viewProjection;
    glm::vec4 rows[4];
    for (int row = 0; row < 4; ++row) rows[row] = glm::vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
    const glm::vec4 planes[6] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
        rows[3] - rows[1], rows[2], rows[3] - rows[2] };
    float planeLengths[6];
    for (int plane = 0; plane < 6; ++plane) planeLengths[plane] = glm::length(glm::vec3(planes[plane]));
    // Pixels per world unit at view depth 1: half the viewport height times
    // the projection's vertical focal scale. The view rotation has unit
    // rows, so the clip-y row length over the clip-w row length recovers it.
    const float focal = glm::length(glm::vec3(rows[1])) / std::max(1.e-6f, glm::length(glm::vec3(rows[3])));
    const float pixelsPerUnit = input.viewportHeight * .5f * focal;

    std::vector<Placement> placements;
    size_t total = 0;
    for (const auto& group : input.groups) total += group.transforms.size();
    placements.reserve(total);
    for (uint32_t groupIndex = 0; groupIndex < input.groups.size(); ++groupIndex)
    {
        const auto& group = input.groups[groupIndex];
        if (group.bands.empty()) continue;
        const float worldScale = std::max({ glm::length(glm::vec3(group.world[0])),
            glm::length(glm::vec3(group.world[1])), glm::length(glm::vec3(group.world[2])) });
        const glm::vec4 center(group.localCenter, 1.f);
        for (const auto& transform : group.transforms)
        {
            const auto& r = transform.rows;
            const glm::vec3 localOrigin(r[0].w, r[1].w, r[2].w);
            const glm::vec3 localCenter(glm::dot(r[0], center), glm::dot(r[1], center), glm::dot(r[2], center));
            const float scale = glm::length(glm::vec3(r[0]));
            const glm::vec4 worldCenter = group.world * glm::vec4(localCenter, 1.f);
            const glm::vec3 worldOrigin = glm::vec3(group.world * glm::vec4(localOrigin, 1.f));
            const float radius = group.localRadius * scale * worldScale;
            bool visible = true;
            for (int plane = 0; plane < 6 && visible; ++plane)
                if (glm::dot(planes[plane], worldCenter) < -radius * planeLengths[plane]) visible = false;
            if (!visible) continue;
            const glm::vec4 clip = m * worldCenter;
            Placement placement{};
            placement.distance = glm::length(worldOrigin - input.cameraPosition);
            placement.order = std::max(0.f, glm::length(glm::vec3(worldCenter) - input.cameraPosition) - radius);
            placement.group = groupIndex;
            if (clip.w <= radius)
            {
                // The camera is inside or right next to it: it covers the screen.
                placement.x = input.viewportWidth * .5f;
                placement.y = input.viewportHeight * .5f;
                placement.radius = std::max(input.viewportWidth, input.viewportHeight);
            }
            else
            {
                placement.x = (clip.x / clip.w * .5f + .5f) * input.viewportWidth;
                placement.y = (.5f - clip.y / clip.w * .5f) * input.viewportHeight;
                placement.radius = radius * pixelsPerUnit / clip.w;
            }
            placements.push_back(placement);
        }
    }
    std::sort(placements.begin(), placements.end(),
        [](const Placement& a, const Placement& b) { return a.order < b.order; });

    std::array<float, GridWidth * GridHeight> coverage{};
    const float tileWidth = input.viewportWidth / GridWidth;
    const float tileHeight = input.viewportHeight / GridHeight;
    const float tileArea = tileWidth * tileHeight;
    std::array<double, DetailCostCurve::Buckets + 1> difference{};
    double fixed = 0.0, visibilitySum = 0.0;
    for (const auto& placement : placements)
    {
        const auto& group = input.groups[placement.group];
        const float left = placement.x - placement.radius, right = placement.x + placement.radius;
        const float top = placement.y - placement.radius, bottom = placement.y + placement.radius;
        const int x0 = std::clamp(static_cast<int>(left / tileWidth), 0, GridWidth - 1);
        const int x1 = std::clamp(static_cast<int>(right / tileWidth), 0, GridWidth - 1);
        const int y0 = std::clamp(static_cast<int>(top / tileHeight), 0, GridHeight - 1);
        const int y1 = std::clamp(static_cast<int>(bottom / tileHeight), 0, GridHeight - 1);
        // How much of it is already covered by what is in front.
        double covered = 0.0, weight = 0.0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const float overlapX = std::min(right, (x + 1) * tileWidth) - std::max(left, x * tileWidth);
                const float overlapY = std::min(bottom, (y + 1) * tileHeight) - std::max(top, y * tileHeight);
                const double area = std::max(0.f, overlapX) * std::max(0.f, overlapY);
                covered += coverage[y * GridWidth + x] * area;
                weight += area;
            }
        const double visibility = weight > 0.0 ? 1.0 - covered / weight : 1.0;
        visibilitySum += visibility;
        // Then it covers its share of each tile (a disc fills pi/4 of its square).
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const float overlapX = std::min(right, (x + 1) * tileWidth) - std::max(left, x * tileWidth);
                const float overlapY = std::min(bottom, (y + 1) * tileHeight) - std::max(top, y * tileHeight);
                const float share = std::min(1.f, std::max(0.f, overlapX) * std::max(0.f, overlapY) / tileArea *
                    .785f * group.opacity);
                float& tile = coverage[y * GridWidth + x];
                tile = 1.f - (1.f - tile) * (1.f - share);
            }
        if (visibility <= 0.0) continue;
        const float logDistance = std::log(std::max(placement.distance, .01f));
        for (const auto& band : group.bands)
        {
            const double cost = band.triangles * visibility;
            if (group.weight <= 0.f)
            {
                if (placement.distance >= band.start && placement.distance < band.end) fixed += cost;
                continue;
            }
            // Band drawn when start * s^w <= d < end * s^w, i.e. for log s in (a, c].
            const float a = band.end >= 1.e19f ? -std::numeric_limits<float>::infinity()
                : (logDistance - std::log(band.end)) / group.weight;
            const float c = band.start <= 0.f ? std::numeric_limits<float>::infinity()
                : (logDistance - std::log(band.start)) / group.weight;
            const int first = FirstBucketAbove(a), last = FirstBucketAbove(c);
            if (first >= last) continue;
            difference[first] += cost;
            difference[last] -= cost;
        }
    }
    double running = 0.0;
    for (int index = 0; index < DetailCostCurve::Buckets; ++index)
    {
        running += difference[index];
        curve.cost[index] = std::max(0.0, running) + fixed;
    }
    curve.placements = static_cast<uint32_t>(placements.size());
    curve.occludedFraction = placements.empty() ? 0.0 : 1.0 - visibilitySum / placements.size();
    curve.valid = !placements.empty();
    return curve;
}
}
