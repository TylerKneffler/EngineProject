#pragma once

#include "Core/Components/Terrain/TerrainVolumeBuild.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Engine::Components
{
// The terrain as one field (todo.md "Terrain: volume-first conversion", step
// 1): signed density in the terrain's own space, positive inside the ground,
// negative in the air, so the surface is where it crosses zero. Every query
// of "where is the ground" goes through here instead of a cell's heights, so
// sculpted rock (overhangs, arches, carved holes) counts wherever it is.
//
// The density is the one VolumeTerrain meshes (TerrainVolumeBuild.cpp):
// the seed ground (h(x, z) - y) * max(normal.y, 0.05), then the cell's
// volume edits and those of its eight neighbours that reach it, in that
// order (FeedVolumeNeighbours), each smoothly subtracted or added. It is
// evaluated exactly, never sampled from a built volume, so a query costs no
// more than the old height read where a cell has no edits.
//
// Cells are cellsX x cellsY samples, cell (x, z) spans [x * span, (x + 1) *
// span) on X and Z. The seed gives a cell's (cellsX + 1) * (cellsY + 1)
// heights (today the baked height archive plus height edits); the edit
// source gives a cell's volume edits in the cell's own space. Both must be
// callable from any thread. Cells are cached (least recently used first out).
//
// Thread-safe: queries may run on any thread at once. A field is configured
// once; callers that change the source invalidate cells, or make a new
// field and hand out the new shared_ptr.
struct TerrainFieldLayout
{
    int cellsX = 32, cellsY = 32;
    float cellWidth = 4.f, cellDepth = 4.f;
    float SpanX() const { return cellsX * cellWidth; }
    float SpanZ() const { return cellsY * cellDepth; }
};

class TerrainField
{
public:
    using Cell = std::pair<int, int>;
    using HeightSeed = std::function<bool(Cell, std::vector<float>& heights)>;
    using EditSource = std::function<bool(Cell, std::vector<TerrainVolumeEdit>& edits)>;

    TerrainField(TerrainFieldLayout layout, HeightSeed seed, EditSource edits,
        size_t cachedCells = 2048);

    const TerrainFieldLayout& Layout() const { return m_layout; }
    Cell CellAt(float x, float z) const;

    // Density in metres at a point (positive in the ground). Cells the seed
    // cannot give read as air.
    float Density(const glm::vec3& position) const;
    // Unit normal of the surface through a point (pointing into the air).
    glm::vec3 Normal(const glm::vec3& position) const;

    struct Surface
    {
        float height = 0.f;
        glm::vec3 normal{0.f, 1.f, 0.f};
    };
    // The first air-to-ground crossing going down the vertical line at
    // (x, z), starting at `from` (default: above everything, so the top
    // surface under open sky). Ground under an arch: start below the arch.
    // None where the line meets no ground below `from` (a cell the seed
    // cannot give, or `from` inside solid ground with nothing open below).
    std::optional<Surface> SurfaceAt(float x, float z,
        float from = std::numeric_limits<float>::infinity()) const;

    struct Hit
    {
        float distance = 0.f;
        glm::vec3 position{0.f};
        glm::vec3 normal{0.f, 1.f, 0.f};
    };
    // First air-to-ground crossing along a ray (direction need not be unit).
    std::optional<Hit> Raycast(const glm::vec3& origin, const glm::vec3& direction,
        float maxDistance) const;

    // Whether any volume edit reaches a cell (own or a neighbour's). Cells
    // without one are their seed's height field.
    bool HasEdits(Cell cell) const;
    // The seed's heights of a cell (nullptr when the seed cannot give it).
    std::shared_ptr<const std::vector<float>> SeedHeights(Cell cell) const;

    // Forget cached cells whose seed or edits changed (and the neighbours
    // their edits reach).
    void Invalidate(Cell cell);
    void InvalidateAll();

private:
    struct CellData
    {
        bool valid = false;
        std::shared_ptr<const std::vector<float>> heights;
        float lowest = 0.f, highest = 0.f;
        // Own edits, then each neighbour's reaching in (dz, dx order), in
        // this cell's space; their joint bounds (empty: no edits).
        std::vector<TerrainVolumeEdit> edits;
        glm::vec3 editLow{std::numeric_limits<float>::max()};
        glm::vec3 editHigh{std::numeric_limits<float>::lowest()};
    };
    // A query walks across cells: it keeps the last cell it read.
    struct Cursor
    {
        Cell cell{std::numeric_limits<int>::min(), 0};
        std::shared_ptr<const CellData> data;
    };
    std::shared_ptr<const CellData> Get(Cell cell) const;
    const CellData* Fetch(Cursor& cursor, Cell cell) const;
    std::shared_ptr<const std::vector<TerrainVolumeEdit>> OwnEdits(Cell cell) const;
    float DensityAt(Cursor& cursor, const glm::vec3& position) const;
    glm::vec3 NormalAt(Cursor& cursor, const glm::vec3& position) const;

    TerrainFieldLayout m_layout;
    HeightSeed m_seed;
    EditSource m_editSource;
    size_t m_capacity;
    mutable std::mutex m_mutex;
    mutable std::list<Cell> m_order; // most recent first
    struct Entry
    {
        std::shared_ptr<const CellData> data;
        std::list<Cell>::iterator position;
    };
    mutable std::map<Cell, Entry> m_cells;
    mutable std::map<Cell, std::shared_ptr<const std::vector<TerrainVolumeEdit>>> m_ownEdits;
};
}
