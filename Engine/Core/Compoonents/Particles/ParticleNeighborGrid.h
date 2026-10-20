#pragma once

#include "ParticleState.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Engine::Components
{
// CPU reference neighbor search for particle solvers. The build cell size is
// the smoothing radius. Build snapshots active positions; rebuild after
// particles move. Queries return source indices in stable ascending order,
// independent of hash-table iteration order.
class ParticleNeighborGrid
{
public:
    bool Build(const ParticleState* particles, size_t count,
        float smoothingRadius);
    void Clear();
    void QueryRadius(const glm::vec3& position, float radius,
        std::vector<uint32_t>& indices,
        uint32_t excludeIndex = std::numeric_limits<uint32_t>::max()) const;
    size_t GetParticleCount() const { return m_entries.size(); }
    size_t GetCellCount() const { return m_cells.size(); }

private:
    struct Cell
    {
        int64_t x = 0, y = 0, z = 0;
        bool operator==(const Cell& other) const
        { return x == other.x && y == other.y && z == other.z; }
    };
    struct CellHash
    {
        size_t operator()(const Cell& cell) const;
    };
    struct Entry
    {
        glm::vec3 position{0.f};
        uint32_t sourceIndex = 0;
    };

    Cell CellFor(double x, double y, double z) const;
    float m_cellSize = 1.f;
    std::vector<Entry> m_entries;
    std::unordered_map<Cell, std::vector<uint32_t>, CellHash> m_cells;
};
}
