#pragma once

#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Far-cell detail (2026-11-24). A far cell used to draw as two triangles
// between its corner heights. The landscape demo's cells hide a median 9 m and
// up to 200 m of relief under that chord, so distant ridgelines were wrong and
// a cell popped by tens of metres when it streamed in at full detail (a bare
// rock crest rising in front of the forest on the meadow walk, frame ~894).
// Each far cell of a finest-level far tile now draws as an L x L grid of the
// height archive's own samples, L the coarsest of 1, 4, 8 or full resolution
// whose height error stays under the allowed screen error at its distance.
// Cells bordering the loaded area always use full resolution, so a cell that
// streams in already looks like its full-detail self.
namespace Engine::Components::FarCellDetail
{
// The whole height archive in memory: cells decode on any thread.
struct Archive
{
    std::vector<char> bytes;
    TerrainHeightArchive::Layout layout;
    int extent = 0, cellsX = 0, cellsY = 0;

    bool Load(const std::string& path, uint64_t signature, int expectedExtent,
        int expectedX, int expectedY)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        const std::streamoff size = file.tellg();
        file.seekg(0);
        if (!TerrainHeightArchive::ReadLayout(file, layout, signature,
                expectedExtent, expectedX, expectedY) ||
            size < static_cast<std::streamoff>(layout.TotalBytes())) return false;
        bytes.resize(static_cast<size_t>(size));
        file.seekg(0);
        if (!file.read(bytes.data(), size)) return false;
        extent = expectedExtent; cellsX = expectedX; cellsY = expectedY;
        return true;
    }

    // Full-resolution heights of one cell ((cellsX + 1) * (cellsY + 1)).
    bool ReadCell(int x, int y, std::vector<float>& heights) const
    {
        if (bytes.empty() || std::abs(x) > extent || std::abs(y) > extent) return false;
        const int stride = layout.Stride(x, y);
        const uint64_t count = TerrainHeightArchive::Layout::StoredSamples(cellsX, cellsY, stride);
        const std::streamoff offset = layout.CellOffset(x, y);
        if (offset < 0 || static_cast<uint64_t>(offset) + count * sizeof(int16_t) > bytes.size())
            return false;
        std::vector<int16_t> stored(static_cast<size_t>(count));
        std::memcpy(stored.data(), bytes.data() + offset, stored.size() * sizeof(int16_t));
        std::vector<int16_t> samples;
        if (stride == 1) samples = std::move(stored);
        else
        {
            std::vector<uint8_t> codes, storedCodes(stored.size(), 0);
            TerrainHeightArchive::Upsample(stored, storedCodes, cellsX, cellsY, stride, samples, codes);
        }
        heights.resize(samples.size());
        for (size_t i = 0; i < samples.size(); ++i)
            heights[i] = static_cast<float>(samples[i]) / layout.scale;
        return true;
    }
};

// Segments per side a cell can draw with; the last is full resolution.
inline std::array<int, 4> Levels(int cells) { return {1, 4, 8, cells}; }

// Largest height error (m) of drawing a cell as 1, 4 and 8 segments per side,
// each quad split along (x + 1, y)-(x, y + 1) as the far tiles are.
inline std::array<float, 3> MeasureCell(const std::vector<float>& heights, int cells)
{
    std::array<float, 3> result{};
    const int width = cells + 1;
    const auto at = [&](int col, int row) { return heights[static_cast<size_t>(row) * width + col]; };
    const std::array<int, 3> levels{1, 4, 8};
    for (size_t level = 0; level < levels.size(); ++level)
    {
        const int segments = levels[level];
        const int step = cells / segments;
        float worst = 0.f;
        for (int row = 0; row <= cells; ++row)
        {
            const int j = std::min(row / step, segments - 1);
            const float fy = static_cast<float>(row - j * step) / step;
            for (int col = 0; col <= cells; ++col)
            {
                const int i = std::min(col / step, segments - 1);
                const float fx = static_cast<float>(col - i * step) / step;
                const float h00 = at(i * step, j * step), h10 = at((i + 1) * step, j * step);
                const float h01 = at(i * step, (j + 1) * step), h11 = at((i + 1) * step, (j + 1) * step);
                const float surface = fx + fy <= 1.f
                    ? h00 + fx * (h10 - h00) + fy * (h01 - h00)
                    : h11 + (1.f - fx) * (h01 - h11) + (1.f - fy) * (h10 - h11);
                worst = std::max(worst, std::abs(at(col, row) - surface));
            }
        }
        result[level] = worst;
    }
    return result;
}

// Per-cell errors over the whole archive, measured once on a worker.
struct Errors
{
    int extent = 0;
    std::vector<std::array<float, 3>> values;
    const std::array<float, 3>* At(int x, int y) const
    {
        if (std::abs(x) > extent || std::abs(y) > extent) return nullptr;
        const int side = extent * 2 + 1;
        return &values[static_cast<size_t>(y + extent) * side + x + extent];
    }
};

inline std::shared_ptr<const Errors> MeasureArchive(const Archive& archive)
{
    if (archive.cellsX != archive.cellsY || archive.cellsX % 8 != 0) return nullptr;
    auto errors = std::make_shared<Errors>();
    errors->extent = archive.extent;
    const int side = archive.extent * 2 + 1;
    errors->values.resize(static_cast<size_t>(side) * side);
    std::vector<float> heights;
    for (int y = -archive.extent; y <= archive.extent; ++y)
        for (int x = -archive.extent; x <= archive.extent; ++x)
        {
            auto& value = errors->values[static_cast<size_t>(y + archive.extent) * side + x + archive.extent];
            value = archive.ReadCell(x, y, heights) ? MeasureCell(heights, archive.cellsX)
                : std::array<float, 3>{0.f, 0.f, 0.f};
        }
    return errors;
}

// Coarsest level whose error is within the allowed height error (m).
inline int ChooseLevel(const std::array<float, 3>& error, float allowed, int cells)
{
    if (error[0] <= allowed) return 1;
    if (error[1] <= allowed) return 4;
    if (error[2] <= allowed) return 8;
    return cells;
}

// One far cell as a level x level grid. Sides are 0 left (u = 0, neighbour
// x - 1), 1 (v = 1, neighbour y + 1), 2 right (u = 1, x + 1), 3 (v = 0, y - 1),
// as the far tiles' seams. A side bordering a loaded cell takes that cell's
// own edge samples (nearEdges, level must be full resolution); any other side
// lies on its edge's polyline at edgeLevels[side] segments, so the coarser of
// two neighbours decides and their shared edge matches without cracks. Edge
// segment ends at the cell corners are the far grid's corner heights.
struct CellGrid
{
    int cellX = 0, cellY = 0;
    int level = 1, cells = 32;
    float cellWidth = 4.f, cellDepth = 4.f;
    std::array<int, 4> edgeLevels{1, 1, 1, 1};
    std::array<const std::vector<float>*, 4> nearEdges{};
    const std::vector<float>* heights = nullptr; // archive samples of this cell
    std::array<float, 4> corners{}; // far grid: (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1)
};

// finish(vertex, height, col, row, u, v) sets colour (and seam blending);
// position (tile-local), normal and uv are set by the caller's place().
template<class Place, class Finish>
void AppendCellGrid(const CellGrid& cell, std::vector<Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices, Place&& place, Finish&& finish)
{
    const int cells = cell.cells, level = cell.level, step = cells / level;
    const int width = cells + 1;
    const auto& h = *cell.heights;
    const auto sample = [&](int col, int row) {
        return h[static_cast<size_t>(std::clamp(row, 0, cells)) * width + std::clamp(col, 0, cells)];
    };
    const auto nearSample = [&](int side, int along) -> float {
        const auto& edge = *cell.nearEdges[static_cast<size_t>(side)];
        // The neighbour's samples on the shared edge.
        return side == 0 ? edge[static_cast<size_t>(along) * width + cells]
            : side == 1 ? edge[static_cast<size_t>(along)]
            : side == 2 ? edge[static_cast<size_t>(along) * width]
            : edge[static_cast<size_t>(cells) * width + along];
    };
    // Corner heights by grid corner (col, row in samples).
    const auto corner = [&](int col, int row) {
        const int index = (col == 0 ? 0 : 1) + (row == 0 ? 0 : 2);
        // A loaded neighbour on either side of this corner owns it.
        const int sideU = col == 0 ? 0 : 2, sideV = row == 0 ? 3 : 1;
        if (cell.nearEdges[static_cast<size_t>(sideU)]) return nearSample(sideU, row);
        if (cell.nearEdges[static_cast<size_t>(sideV)]) return nearSample(sideV, col);
        return cell.corners[static_cast<size_t>(index)];
    };
    // Height on a side at sample 'along' (0..cells) from its start.
    const auto sideHeight = [&](int side, int along) -> float {
        if (cell.nearEdges[static_cast<size_t>(side)]) return nearSample(side, along);
        const int segments = std::max(1, cell.edgeLevels[static_cast<size_t>(side)]);
        const int segmentSamples = cells / segments;
        const int k = std::min(along / segmentSamples, segments - 1);
        const int a = k * segmentSamples, b = a + segmentSamples;
        const auto edgeAt = [&](int s) {
            const int col = side == 0 ? 0 : side == 2 ? cells : s;
            const int row = side == 3 ? 0 : side == 1 ? cells : s;
            if ((col == 0 || col == cells) && (row == 0 || row == cells)) return corner(col, row);
            return sample(col, row);
        };
        const float t = static_cast<float>(along - a) / segmentSamples;
        return edgeAt(a) + (edgeAt(b) - edgeAt(a)) * t;
    };
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (int j = 0; j <= level; ++j)
        for (int i = 0; i <= level; ++i)
        {
            const int col = i * step, row = j * step;
            float height;
            if ((i == 0 || i == level) && (j == 0 || j == level)) height = corner(col, row);
            else if (i == 0) height = sideHeight(0, row);
            else if (i == level) height = sideHeight(2, row);
            else if (j == 0) height = sideHeight(3, col);
            else if (j == level) height = sideHeight(1, col);
            else height = sample(col, row);
            // Normals at this level's spacing, so coarse cells shade as smoothly as they are.
            const int reach = std::max(1, step);
            const glm::vec3 normal = glm::normalize(glm::vec3(
                (sample(col - reach, row) - sample(col + reach, row)) /
                    (static_cast<float>(std::min(col, reach) + std::min(cells - col, reach)) * cell.cellWidth),
                1.f,
                (sample(col, row - reach) - sample(col, row + reach)) /
                    (static_cast<float>(std::min(row, reach) + std::min(cells - row, reach)) * cell.cellDepth)));
            Mesh::Vertex vertex{};
            const float u = static_cast<float>(col) / cells, v = static_cast<float>(row) / cells;
            place(vertex, u, v, height, normal);
            finish(vertex, height, col, row, u, v);
            vertices.push_back(vertex);
        }
    for (int j = 0; j < level; ++j)
        for (int i = 0; i < level; ++i)
        {
            const uint32_t a = base + static_cast<uint32_t>(j * (level + 1) + i);
            const uint32_t b = a + static_cast<uint32_t>(level + 1);
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    // A skirt under every side that borders a loaded cell: a strip hanging
    // SkirtDepth straight down, both faces. A loaded cell drawn as a volume
    // does not follow this edge exactly (surface nets places its vertices
    // inside the cells, coarse levels 16 m apart), and the gap between them
    // showed the lake plane below as a bright slit (MeadowSlope, 2026-11-24).
    constexpr float SkirtDepth = 8.f;
    const auto gridIndex = [&](int i, int j) { return base + static_cast<uint32_t>(j * (level + 1) + i); };
    for (int side = 0; side < 4; ++side)
    {
        if (!cell.nearEdges[static_cast<size_t>(side)]) continue;
        std::vector<uint32_t> top, bottom;
        for (int k = 0; k <= level; ++k)
        {
            const int i = side == 0 ? 0 : side == 2 ? level : k;
            const int j = side == 3 ? 0 : side == 1 ? level : k;
            const uint32_t upper = gridIndex(i, j);
            Mesh::Vertex lowered = vertices[upper];
            lowered.pos[1] -= SkirtDepth;
            top.push_back(upper);
            bottom.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(lowered);
        }
        for (int k = 0; k < level; ++k)
            indices.insert(indices.end(), {
                top[k], bottom[k], top[k + 1], top[k + 1], bottom[k], bottom[k + 1],
                top[k], top[k + 1], bottom[k], top[k + 1], bottom[k + 1], bottom[k]});
    }
}
}
