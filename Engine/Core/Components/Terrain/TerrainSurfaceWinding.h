#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace Engine::Components
{
// Terrain is mostly a height field. Normalize legacy/cached triangles as well
// as new geometry; vertical skirt triangles retain their authored orientation,
// and so do triangles whose winding agrees with their vertex normals: the
// undersides of overhangs in terrain stamps (TerrainStamp) face down on
// purpose. Height-field vertices always have upward normals, so a grid
// triangle wound downward is still turned up.
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
        if (up >= 0.0) continue;
        const double ex = double(b.pos[0]) - a.pos[0], ey = double(b.pos[1]) - a.pos[1],
            ez = double(b.pos[2]) - a.pos[2];
        const double fx = double(c.pos[0]) - a.pos[0], fy = double(c.pos[1]) - a.pos[1],
            fz = double(c.pos[2]) - a.pos[2];
        const double nx = ey * fz - ez * fy, ny = ez * fx - ex * fz, nz = ex * fy - ey * fx;
        const double agree = nx * (double(a.normal[0]) + b.normal[0] + c.normal[0]) +
            ny * (double(a.normal[1]) + b.normal[1] + c.normal[1]) +
            nz * (double(a.normal[2]) + b.normal[2] + c.normal[2]);
        if (agree < 0.0) std::swap(indices[i + 1], indices[i + 2]);
    }
}
}
