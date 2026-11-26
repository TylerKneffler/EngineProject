#include "Core/Components/Terrain/TerrainTrails.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// CPU twin of Object.hlsl TerrainTrailSample (without the pixel footprint):
// keep the two in step.
namespace Engine::Components
{
namespace
{
float SmoothStep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    const glm::vec2 ab = b - a;
    const float lengthSquared = glm::dot(ab, ab);
    const float t = lengthSquared > 0.f ? std::clamp(glm::dot(p - a, ab) / lengthSquared, 0.f, 1.f) : 0.f;
    return glm::length(p - (a + ab * t));
}

void Mix(uint64_t& hash, const void* data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
}

float EdgeOf(const TerrainTrail& trail) { return std::max(trail.edge, trail.core + .01f); }
}

std::shared_ptr<const TerrainTrailNetwork> TerrainTrailNetwork::Build(std::vector<TerrainTrail> trails)
{
    auto network = std::make_shared<TerrainTrailNetwork>();
    network->m_trails = std::move(trails);
    // Order-independent: publishers register in any order (keyed by
    // address), and the signature is part of the cell cache key.
    uint64_t signature = 0;
    glm::vec2 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
    for (uint32_t index = 0; index < network->m_trails.size(); ++index)
    {
        const auto& trail = network->m_trails[index];
        uint64_t hash = 1469598103934665603ull;
        Mix(hash, &trail.core, sizeof(trail.core)); Mix(hash, &trail.edge, sizeof(trail.edge));
        Mix(hash, &trail.strength, sizeof(trail.strength)); Mix(hash, &trail.depth, sizeof(trail.depth));
        Mix(hash, &trail.layer, sizeof(trail.layer));
        Mix(hash, &trail.crossableLayers, sizeof(trail.crossableLayers));
        Mix(hash, &trail.farColor, sizeof(trail.farColor));
        if (!trail.points.empty())
            Mix(hash, trail.points.data(), trail.points.size() * sizeof(glm::vec2));
        hash ^= hash >> 29; hash *= 0xbf58476d1ce4e5b9ull; hash ^= hash >> 32;
        signature += hash;
        if (trail.strength <= 0.f && trail.depth <= 0.f) continue;
        const float reach = EdgeOf(trail) + BinMargin;
        for (size_t i = 1; i < trail.points.size(); ++i)
        {
            const glm::vec2 a = trail.points[i - 1], b = trail.points[i];
            network->m_segments.push_back({a, b, index});
            low = glm::min(low, glm::min(a, b) - reach);
            high = glm::max(high, glm::max(a, b) + reach);
        }
    }
    network->m_signature = signature | 1ull;
    if (network->m_segments.empty()) return network;

    // Bins: grow the side until the grid fits MaxBins.
    float binSize = BinSize;
    const glm::vec2 extent = high - low;
    while (std::ceil(extent.x / binSize) * std::ceil(extent.y / binSize) > MaxBins) binSize *= 2.f;
    network->m_binSize = binSize;
    network->m_origin = low;
    network->m_binsX = std::max(1, static_cast<int>(std::ceil(extent.x / binSize)));
    network->m_binsZ = std::max(1, static_cast<int>(std::ceil(extent.y / binSize)));
    const size_t binCount = static_cast<size_t>(network->m_binsX) * network->m_binsZ;
    const auto visit = [&](const Segment& segment, auto&& add)
    {
        const float reach = EdgeOf(network->m_trails[segment.trail]) + BinMargin;
        const glm::vec2 a = (glm::min(segment.a, segment.b) - reach - low) / binSize;
        const glm::vec2 b = (glm::max(segment.a, segment.b) + reach - low) / binSize;
        const int x0 = std::clamp(static_cast<int>(std::floor(a.x)), 0, network->m_binsX - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor(b.x)), 0, network->m_binsX - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor(a.y)), 0, network->m_binsZ - 1);
        const int z1 = std::clamp(static_cast<int>(std::floor(b.y)), 0, network->m_binsZ - 1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                // Only bins the segment's reach touches (long diagonal
                // segments cross few of their bounding box's bins).
                const glm::vec2 centre = low + (glm::vec2(x, z) + .5f) * binSize;
                if (SegmentDistance(centre, segment.a, segment.b) <= reach + binSize * .7072f)
                    add(static_cast<size_t>(z) * network->m_binsX + x);
            }
    };
    std::vector<uint32_t> counts(binCount + 1, 0);
    for (const auto& segment : network->m_segments) visit(segment, [&](size_t bin) { ++counts[bin]; });
    network->m_binStart.assign(binCount + 1, 0);
    for (size_t bin = 0; bin < binCount; ++bin)
        network->m_binStart[bin + 1] = network->m_binStart[bin] + counts[bin];
    network->m_binList.resize(network->m_binStart[binCount]);
    std::vector<uint32_t> fill(network->m_binStart.begin(), network->m_binStart.end() - 1);
    for (uint32_t index = 0; index < network->m_segments.size(); ++index)
        visit(network->m_segments[index], [&](size_t bin) { network->m_binList[fill[bin]++] = index; });
    return network;
}

TerrainTrailNetwork::Sample TerrainTrailNetwork::Evaluate(float x, float z) const
{
    Sample result;
    if (m_segments.empty()) return result;
    const int bx = static_cast<int>(std::floor((x - m_origin.x) / m_binSize));
    const int bz = static_cast<int>(std::floor((z - m_origin.y) / m_binSize));
    if (bx < 0 || bz < 0 || bx >= m_binsX || bz >= m_binsZ) return result;
    const size_t bin = static_cast<size_t>(bz) * m_binsX + bx;
    const glm::vec2 p(x, z);
    for (uint32_t entry = m_binStart[bin]; entry < m_binStart[bin + 1]; ++entry)
    {
        const auto& segment = m_segments[m_binList[entry]];
        const auto& trail = m_trails[segment.trail];
        const float metres = SegmentDistance(p, segment.a, segment.b);
        result.metres = std::min(result.metres, metres);
        const float track = 1.f - SmoothStep(trail.core, EdgeOf(trail), metres);
        const float share = track * std::clamp(trail.strength, 0.f, 1.f);
        if (track > 0.f && (share > result.share || (share == result.share && track > result.track)))
        {
            result.share = share;
            result.track = track;
            result.trail = static_cast<int>(segment.trail);
        }
    }
    return result;
}

bool TerrainTrailNetwork::CarvesArea(glm::vec2 low, glm::vec2 high) const
{
    for (const auto& segment : m_segments)
    {
        const auto& trail = m_trails[segment.trail];
        if (trail.depth == 0.f) continue;
        const float reach = EdgeOf(trail);
        if (std::max(segment.a.x, segment.b.x) + reach >= low.x &&
            std::min(segment.a.x, segment.b.x) - reach <= high.x &&
            std::max(segment.a.y, segment.b.y) + reach >= low.y &&
            std::min(segment.a.y, segment.b.y) - reach <= high.y)
            return true;
    }
    return false;
}

bool TerrainTrailNetwork::Carve(float originX, float originZ, int countX, int countY, float cellWidth,
    float cellDepth, const std::vector<float>& weights, size_t layerCount, std::vector<float>& heights) const
{
    if (m_segments.empty() || heights.size() != static_cast<size_t>(countX + 1) * (countY + 1) ||
        !CarvesArea({originX, originZ}, {originX + countX * cellWidth, originZ + countY * cellDepth}))
        return false;
    const bool painted = layerCount > 0 && weights.size() == heights.size() * layerCount;
    bool changed = false;
    for (int row = 0; row <= countY; ++row)
        for (int col = 0; col <= countX; ++col)
        {
            const size_t i = static_cast<size_t>(row) * (countX + 1) + col;
            const Sample sample = Evaluate(originX + col * cellWidth, originZ + row * cellDepth);
            if (sample.trail < 0) continue;
            const auto& trail = m_trails[static_cast<size_t>(sample.trail)];
            if (trail.depth == 0.f) continue;
            // Only over the layers the track may cross (as it is shaded).
            float crossable = 1.f;
            if (painted)
            {
                float total = 0.f, allowed = 0.f;
                for (size_t layer = 0; layer < layerCount; ++layer)
                {
                    const float w = weights[i * layerCount + layer];
                    total += w;
                    if (layer < 32 && (trail.crossableLayers & (1u << layer))) allowed += w;
                }
                crossable = total > 1.e-5f ? allowed / total : 0.f;
            }
            const float sink = trail.depth * sample.track * crossable;
            if (sink == 0.f) continue;
            heights[i] -= sink;
            changed = true;
        }
    return changed;
}

TerrainTrailNetwork::GpuLayout TerrainTrailNetwork::PackGpu(const glm::vec3* layerFarColors,
    size_t layerColorCount) const
{
    GpuLayout layout;
    if (m_segments.empty()) return layout;
    const size_t binCount = static_cast<size_t>(m_binsX) * m_binsZ;
    const size_t binTexels = (binCount + 1 + 3) / 4;
    const size_t listTexels = (m_binList.size() + 3) / 4;
    const size_t segmentTexels = m_segments.size() * 3;
    const size_t total = binTexels + listTexels + segmentTexels;
    layout.width = 2048;
    layout.height = static_cast<uint32_t>((total + layout.width - 1) / layout.width);
    layout.texels.assign(static_cast<size_t>(layout.width) * layout.height * 4, 0.f);
    float* texels = layout.texels.data();
    for (size_t bin = 0; bin <= binCount; ++bin)
        texels[bin] = static_cast<float>(m_binStart[bin]);
    float* list = texels + binTexels * 4;
    for (size_t entry = 0; entry < m_binList.size(); ++entry)
        list[entry] = static_cast<float>(m_binList[entry]);
    float* segments = texels + (binTexels + listTexels) * 4;
    for (size_t index = 0; index < m_segments.size(); ++index)
    {
        const auto& segment = m_segments[index];
        const auto& trail = m_trails[segment.trail];
        float* out = segments + index * 12;
        out[0] = segment.a.x; out[1] = segment.a.y; out[2] = segment.b.x; out[3] = segment.b.y;
        const int layer = std::clamp(trail.layer, 0, 5);
        out[4] = trail.core; out[5] = EdgeOf(trail); out[6] = std::clamp(trail.strength, 0.f, 1.f);
        out[7] = static_cast<float>(layer * 64 + static_cast<int>(trail.crossableLayers & 63u));
        glm::vec3 farColor = trail.farColor;
        if (farColor.x < 0.f || farColor.y < 0.f || farColor.z < 0.f)
            farColor = layerFarColors && static_cast<size_t>(layer) < layerColorCount
                ? layerFarColors[layer] : glm::vec3(.4f);
        out[8] = farColor.x; out[9] = farColor.y; out[10] = farColor.z; out[11] = 0.f;
    }
    layout.grid = {m_origin.x, m_origin.y, 1.f / m_binSize, static_cast<float>(m_binsX)};
    layout.keys = {static_cast<uint32_t>(m_binsZ), static_cast<uint32_t>(binTexels),
        static_cast<uint32_t>(binTexels + listTexels), layout.width};
    return layout;
}
}
