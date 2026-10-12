#pragma once

#include "WeightPaintSurface.h"
#include <algorithm>
#include <cmath>
#include <queue>

namespace Engine::Editor::WeightPaintBucket
{
// Flood across shared edges, comparing each face's mean weight with the hit
// face. A face is either filled completely or excluded, and output order is
// stable regardless of hash table iteration order in the topology builder.
inline std::vector<uint32_t> ConnectedVertices(
    const WeightPaintSurface::Topology& topology,
    const std::vector<float>& weights, size_t hitFace, float tolerance)
{
    std::vector<uint32_t> result;
    if (weights.size() != topology.vertexCount ||
        hitFace >= topology.faces.size() || !std::isfinite(tolerance) ||
        tolerance < 0.f) return result;
    const auto mean = [&](size_t face)
    {
        const auto& tri = topology.faces[face];
        if (tri[0] >= weights.size() || tri[1] >= weights.size() ||
            tri[2] >= weights.size()) return NAN;
        return (weights[tri[0]] + weights[tri[1]] +
            weights[tri[2]]) / 3.f;
    };
    const float seed = mean(hitFace);
    if (!std::isfinite(seed)) return result;
    std::vector<bool> visited(topology.faces.size(), false);
    std::queue<uint32_t> pending;
    pending.push(static_cast<uint32_t>(hitFace));
    visited[hitFace] = true;
    while (!pending.empty())
    {
        const uint32_t face = pending.front();
        pending.pop();
        const float value = mean(face);
        if (!std::isfinite(value) || std::abs(value - seed) > tolerance)
            continue;
        const auto& tri = topology.faces[face];
        result.insert(result.end(), tri.begin(), tri.end());
        for (uint32_t neighbor : topology.neighbors[face])
            if (!visited[neighbor])
            {
                visited[neighbor] = true;
                pending.push(neighbor);
            }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
}
