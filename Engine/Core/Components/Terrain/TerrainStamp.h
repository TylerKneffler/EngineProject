#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Components
{
// A terrain stamp (tools/bake_terrain_stamp.py): a rock shape as a regular
// grid of 3D surface points, seen along the shape's outward direction, so
// overhangs fit where a height map cannot. A Terrain draws each placed stamp
// as part of its own mesh (Terrain::stamps): the points' weights pull the
// patch's border onto the ground, so it closes onto the terrain all round,
// and the ground's paint is used across it.
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

// A sculpted cell (tools/cliff_sculpt.py): the cell's whole surface, its
// ground taken into the shape of the cliff scans placed in it as one mesh, in
// place of the height grid. Its rock is shaded with a seamless rock material
// (tools/cliff_rock_texture.py) projected from three sides; each vertex has
// the scans' colour there as a tint on it, and a weight, 1 on rock and 0 on
// ground (the paint layers). Levels are simplified triangle lists, finest
// first; the 4 m border points are kept in every level, so the cell meets its
// neighbours' grids.
struct TerrainSculpt
{
    struct Vertex
    {
        glm::vec3 position{0.f};   // cell space
        glm::vec3 normal{0.f, 1.f, 0.f};
        glm::vec3 tint{1.f};
        float weight = 0.f;
    };
    int cellX = 0, cellZ = 0;
    std::string material;          // the rock material
    float repeatsPerMetre = 0.1f;  // the rock material's tiling
    std::vector<Vertex> vertices;
    std::vector<std::vector<uint32_t>> levels;
    // Loads a .sculpt file once per path (thread-safe); null when missing or
    // malformed.
    static std::shared_ptr<const TerrainSculpt> Load(const std::string& path);
};

// One placed stamp, in the terrain cell's local space.
struct TerrainStampInstance
{
    std::shared_ptr<const TerrainStamp> stamp;
    glm::mat4 transform{1.f};
    // Index into the terrain's stampMaterials (the stamp's own surface
    // images), or -1 to draw it with the painted layers only.
    int materialIndex = -1;
    // Set instead of `stamp`: the cell's surface is this sculpt (its rock
    // images are stampMaterials[materialIndex]).
    std::shared_ptr<const TerrainSculpt> sculpt;
};
}
