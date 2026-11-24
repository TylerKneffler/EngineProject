#pragma once

#include "Core/Components/Terrain/TerrainVolume.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Components
{
class Terrain;

// A shape added to or carved out of a volume terrain cell (a cave, an arch,
// an overhang): the edits the height field could not hold. Stored per cell,
// in the cell's space (TerrainManager: <cellDataDirectory>/<x>_<z>.volumeedit),
// and applied in order after the ground, each blended in over
// `blend` metres. An edit belongs to the cell holding its centre; the cells
// around it read it too, so it may cross into them but not past them (less
// than a cell across).
struct TerrainVolumeEdit
{
    enum class Shape { Sphere, Box, Capsule };
    Shape shape = Shape::Sphere;
    bool subtract = true;          // carve out (else add rock)
    // Sphere: centre a. Capsule: from a to b. Box: centre a, half extents b,
    // turned `yaw` degrees about y, its edges rounded by `radius`.
    glm::vec3 a{0.f}, b{0.f};
    float radius = 1.f;
    float yaw = 0.f;
    float blend = 1.f;

    // Signed distance to the shape, negative inside.
    float Distance(const glm::vec3& p) const;
    // Bounds of what the edit can change (the shape plus its blend).
    glm::vec3 Minimum() const;
    glm::vec3 Maximum() const;
    // Moved by `offset` (another cell's space).
    TerrainVolumeEdit Shifted(const glm::vec3& offset) const;
};
// A .volumeedit file: one edit per line, '#' starts a comment.
//   add|subtract sphere  x y z  radius  blend
//   add|subtract capsule ax ay az  bx by bz  radius  blend
//   add|subtract box     x y z  hx hy hz  yaw  rounding  blend
// A missing file is an empty list (true); a malformed line fails.
bool LoadTerrainVolumeEdits(const std::string& path, std::vector<TerrainVolumeEdit>& edits);
bool SaveTerrainVolumeEdits(const std::string& path, const std::vector<TerrainVolumeEdit>& edits);

// What BuildTerrainVolume reads from a terrain: a copy, so a worker thread
// can build while the component lives on.
struct TerrainVolumeInput
{
    int cellsX = 0, cellsY = 0;
    float cellWidth = 1.f, cellDepth = 1.f;
    std::vector<float> heights;
    // The eight neighbours' heights (same grid), index (dz + 1) * 3 + dx + 1;
    // empty where unknown, and the border repeats there. The volume reads
    // past the cell's edge (its margin), so coarse levels follow the real
    // ground across the border and neighbouring cells agree there.
    std::array<std::vector<float>, 9> neighbourHeights;
    std::vector<TerrainVolumeEdit> edits;
    static TerrainVolumeInput From(const Terrain& terrain);
};
std::shared_ptr<TerrainVolume> BuildTerrainVolume(const TerrainVolumeInput& input,
    float spacing = 1.f, float band = 8.f);

// A height-field terrain cell as a density volume (volume terrain stage 3,
// .md/todo.md), the runtime twin of tools/terrain_volume.py:
//   * ground: (height - y) times the surface normal's y, so the density is
//     close to the distance to the ground on slopes too;
//   * then the edits (caves, arches; rock shapes are in the heights, written
//     there by the stamp brushes).
// The grid covers the cell at `spacing` metres, plus a margin past each edge
// that is read but not meshed (TerrainVolume::interiorFirst), from the
// lowest height less `band` to the highest height plus `band`.
// Only chunks near the surface are computed; chunks wholly above or below it
// are uniform (TerrainVolume's sparse storage). Densities are clamped to
// +-band.
std::shared_ptr<TerrainVolume> BuildTerrainVolume(const Terrain& terrain,
    float spacing = 1.f, float band = 8.f);
}
