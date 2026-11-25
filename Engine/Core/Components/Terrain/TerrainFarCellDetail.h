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

// The height archive held in memory, so cells decode on any thread (far
// volume nodes, the terrain field). The name is from the far-cell detail
// levels (2026-11-24) that the far volume nodes replaced.
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

}
