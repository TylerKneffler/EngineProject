#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace Engine::Components
{
// Terrain is a height field. Normalize legacy/cached triangles as well as new
// geometry; vertical skirt triangles retain their authored orientation.
template<class Vertex>
void OrientTerrainSurfaceUp(const std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices)
{
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size()) continue;
        const auto& a = vertices[indices[i]];
        const auto& b = vertices[indices[i + 1]];
        const auto& c = vertices[indices[i + 2]];
        const double up = (double(b.pos[2]) - a.pos[2]) * (double(c.pos[0]) - a.pos[0]) -
            (double(b.pos[0]) - a.pos[0]) * (double(c.pos[2]) - a.pos[2]);
        if (up < 0.0) std::swap(indices[i + 1], indices[i + 2]);
    }
}
}
