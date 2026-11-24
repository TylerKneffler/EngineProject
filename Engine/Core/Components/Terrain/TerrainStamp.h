#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Components
{
// A terrain stamp (tools/bake_terrain_stamp.py): a scanned rock shape as a
// regular grid of 3D surface points with simplified triangle levels. Stamps
// are terrain brushes: TerrainBrushShape rasterizes one from above, and the
// editor's shaped brushes and Stamp tool write it into the height data. They
// are not drawn or placed at runtime.
struct TerrainStamp
{
    struct Point
    {
        glm::vec3 position{0.f};   // stamp space (the source mesh's frame)
        glm::vec3 normal{0.f, 1.f, 0.f};
        glm::vec2 texcoord{0.f};   // the source mesh's UVs (unused by terrain shading)
        float weight = 0.f;        // 0 on and outside the outline, 1 inside
    };
    int width = 0, height = 0;     // points along u, rows along v
    float spacing = 0.f;           // stamp-space metres between points
    glm::vec3 direction{0.f, 0.f, 1.f};
    // The grid's u and v axes (stamp space); u x v = direction.
    glm::vec3 axisU{1.f, 0.f, 0.f}, axisV{0.f, 1.f, 0.f};
    std::string material;          // the source scan's material
    std::vector<Point> points;     // width * height, rows along v
    // Simplified triangle lists over the points (tools/stamp_simplify.py),
    // finest first: point indices, three per triangle, wound out of the face.
    std::vector<std::vector<uint32_t>> levels;

    const Point& At(int u, int v) const { return points[static_cast<size_t>(v) * width + u]; }
    // Loads a .stamp file once per path (thread-safe); null when missing or
    // malformed.
    static std::shared_ptr<const TerrainStamp> Load(const std::string& path);
};
}
