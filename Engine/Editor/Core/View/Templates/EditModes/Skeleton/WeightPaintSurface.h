#pragma once

#include "MirrorWeightPaint.h"
#include <array>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace Engine::Editor::WeightPaintSurface
{
struct Topology
{
    size_t vertexCount = 0;
    std::vector<uint32_t> indices;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<std::vector<uint32_t>> neighbors;
};

inline Topology BuildTopology(const std::vector<uint32_t>& indices,
    size_t vertexCount)
{
    Topology result;
    result.vertexCount = vertexCount;
    result.indices = indices;
    const size_t count = indices.empty() ? vertexCount : indices.size();
    result.faces.resize(count / 3);
    result.neighbors.resize(result.faces.size());
    std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;
    for (size_t face = 0; face < result.faces.size(); ++face)
    {
        auto& corners = result.faces[face];
        for (size_t corner = 0; corner < 3; ++corner)
            corners[corner] = indices.empty()
                ? static_cast<uint32_t>(face * 3 + corner)
                : indices[face * 3 + corner];
        if (corners[0] >= vertexCount || corners[1] >= vertexCount ||
            corners[2] >= vertexCount) continue;
        for (size_t edge = 0; edge < 3; ++edge)
        {
            const uint32_t a = std::min(corners[edge], corners[(edge + 1) % 3]);
            const uint32_t b = std::max(corners[edge], corners[(edge + 1) % 3]);
            edgeFaces[(static_cast<uint64_t>(a) << 32) | b]
                .push_back(static_cast<uint32_t>(face));
        }
    }
    for (const auto& [edge, incident] : edgeFaces)
        for (uint32_t a : incident)
            for (uint32_t b : incident)
                if (a != b) result.neighbors[a].push_back(b);
    return result;
}

// Propagate a stroke only through faces connected to the hit face. Each face
// contributes its actual vertex footprint and a barycentric contribution at
// the closest brush point, so large sparse triangles remain paintable.
template<class Metric, class Falloff, class Visible>
std::vector<float> Coverage(const Topology& topology,
    const std::vector<glm::vec3>& positions, size_t hitFace,
    const glm::vec3& center, float radius, Metric metric,
    Falloff falloff, Visible visible)
{
    std::vector<float> coverage(positions.size(), 0.f);
    if (positions.size() != topology.vertexCount ||
        hitFace >= topology.faces.size() || radius <= 0.f)
        return coverage;
    std::vector<bool> visited(topology.faces.size(), false);
    std::queue<uint32_t> pending;
    pending.push(static_cast<uint32_t>(hitFace));
    visited[hitFace] = true;
    while (!pending.empty())
    {
        const uint32_t face = pending.front();
        pending.pop();
        const auto& corners = topology.faces[face];
        if (corners[0] >= positions.size() ||
            corners[1] >= positions.size() ||
            corners[2] >= positions.size()) continue;
        const auto nearest = MirrorWeightPaint::ClosestPoint(center,
            positions[corners[0]], positions[corners[1]],
            positions[corners[2]]);
        const float distance = metric(nearest.point - center);
        if (distance > radius || !visible(face, nearest)) continue;
        const float faceFalloff = falloff(distance, radius);
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t vertex = corners[corner];
            coverage[vertex] = std::max(coverage[vertex],
                std::max(falloff(metric(positions[vertex] - center), radius),
                    .5f * faceFalloff * nearest.barycentric[corner]));
        }
        for (uint32_t neighbor : topology.neighbors[face])
            if (!visited[neighbor])
            {
                visited[neighbor] = true;
                pending.push(neighbor);
            }
    }
    return coverage;
}
}
