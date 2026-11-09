#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace Engine::Components
{
void TerrainManager::ConfigureFarTerrain(int halfExtentCells,
    std::vector<float> heights, std::vector<TerrainHeightArchive::FarPatch> patches,
    std::string heightArchivePath, uint64_t sourceSignature,
    FarTerrainPalette palette)
{
    ClearFarTerrain();
    m_farHalfExtentCells = halfExtentCells;
    m_farHeights = std::move(heights);
    m_farHeightsSnapshot = std::make_shared<const std::vector<float>>(m_farHeights);
    m_farPatches = std::move(patches);
    for (const auto& patch : m_farPatches)
        m_farPatchCells.emplace(patch.x, patch.y);
    m_farHeightArchivePath = std::move(heightArchivePath);
    m_farSourceSignature = sourceSignature;
    m_farPalette = std::move(palette);
    m_farEnabled = true;
}

void TerrainManager::RefreshFarTerrainEditedCells(
    const std::set<Cell>& changedCells)
{
    if (m_farHeights.empty()) return;
    if (!changedCells.empty())
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        ++m_farPrepareRevision;
        m_farPrepareQueue.clear();
        m_preparedFarTiles.clear();
        for (auto& tile : m_farTiles) tile.preparing = false;
    }
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    for (Cell cell : changedCells)
    {
        if (std::abs(cell.first) > m_farHalfExtentCells ||
            std::abs(cell.second) > m_farHalfExtentCells) continue;
        const Terrain* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != expected) continue;
        std::vector<float> colors;
        if (terrain->weights.size() == expected * 6)
        {
            colors.assign(expected * 3, 0.f);
            for (size_t i = 0; i < expected; ++i)
                for (int layer = 0; layer < 6; ++layer)
                    for (int channel = 0; channel < 3; ++channel)
                        colors[i * 3 + channel] +=
                            terrain->weights[i * 6 + layer] *
                            m_farPalette.nearColors[layer][channel];
        }
        auto patch = std::find_if(m_farPatches.begin(), m_farPatches.end(),
            [&](const auto& item) {
                return item.x == cell.first && item.y == cell.second;
            });
        const bool newPatch = patch == m_farPatches.end();
        size_t patchIndex = newPatch ? m_farPatches.size() :
            static_cast<size_t>(patch - m_farPatches.begin());
        if (patch == m_farPatches.end())
        {
            m_farPatches.push_back({cell.first, cell.second,
                terrain->heights, std::move(colors)});
            m_farPatchCells.insert(cell);
            for (auto& tile : m_farTiles)
                if (tile.lodStride == 1 && cell.first >= tile.startX &&
                    cell.first < tile.endX && cell.second >= tile.startY &&
                    cell.second < tile.endY)
                {
                    tile.patchIndices.push_back(patchIndex);
                    break;
                }
        }
        else
        {
            patch->heights = terrain->heights;
            patch->colors = std::move(colors);
        }
        for (auto& tile : m_farTiles)
            if (cell.first >= tile.startX && cell.first < tile.endX &&
                cell.second >= tile.startY && cell.second < tile.endY)
            {
                tile.containsEditedPatch = true;
                if (!newPatch && tile.lodStride == 1 && tile.built &&
                    tile.patchVertexOffsets.count(patchIndex))
                    tile.dirtyPatchIndices.insert(patchIndex);
                else tile.built = false;
            }
    }
}

void TerrainManager::ClearFarTerrain()
{
    {
        std::lock_guard<std::mutex> lock(m_farPrepareMutex);
        ++m_farPrepareRevision;
        m_farPrepareQueue.clear();
        m_preparedFarTiles.clear();
    }
    if (Owner && Owner->GetScene())
        for (const auto& tile : m_farTiles)
            if (tile.object) Owner->GetScene()->RemoveObject(tile.object);
    m_farTiles.clear();
    m_farHeights.clear();
    m_farHeightsSnapshot.reset();
    m_farPatches.clear();
    m_farPatchCells.clear();
    m_farHeightArchivePath.clear();
    m_farHalfExtentCells = 0;
    m_farSourceSignature = 0;
    m_farTileUploadCount = 0;
    m_farTileCacheHits = 0;
    m_farTileUploadedBytes = 0;
    m_farTileRebuildMilliseconds = 0.0;
    m_farEnabled = false;
}

void TerrainManager::SetFarTerrainEnabled(bool enabled)
{
    if (m_farEnabled == enabled) return;
    m_farEnabled = enabled;
    if (!enabled)
    {
        {
            std::lock_guard<std::mutex> lock(m_farPrepareMutex);
            ++m_farPrepareRevision;
            m_farPrepareQueue.clear();
            m_preparedFarTiles.clear();
        }
        for (auto& tile : m_farTiles) tile.preparing = false;
        for (const auto& tile : m_farTiles)
            if (tile.object) tile.object->enabled = false;
    }
}

std::vector<Mesh*>
TerrainManager::GetFarTileMeshes() const
{
    std::vector<Mesh*> meshes;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.mesh &&
            tile.mesh->IsReady())
            meshes.push_back(tile.mesh);
    return meshes;
}

size_t TerrainManager::GetVisibleFarTileCount() const
{
    size_t count = 0;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.hasGeometry) ++count;
    return count;
}

size_t TerrainManager::GetVisibleFarTileCountForStride(int stride) const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [stride](const FarTile& tile) {
            return tile.lodStride == stride && tile.object &&
                tile.object->enabled && tile.hasGeometry;
        });
}

size_t TerrainManager::GetDesiredFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.desired; });
}

size_t TerrainManager::GetBuiltFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.built; });
}

bool TerrainManager::AreDesiredFarTilesBuilt() const
{
    return std::all_of(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return !tile.desired || tile.built; });
}

size_t TerrainManager::CountVisibleCoverageGaps(int centerX, int centerY,
    int radius) const
{
    size_t gaps = 0;
    radius = std::clamp(radius, 0, 32);
    for (int y = centerY - radius; y <= centerY + radius; ++y)
        for (int x = centerX - radius; x <= centerX + radius; ++x)
        {
            if (!HasCell(x, y)) continue;
            const Terrain* nearTerrain = GetLoadedCell(x, y);
            if (nearTerrain && nearTerrain->Owner &&
                nearTerrain->Owner->enabled)
                if (const Mesh* mesh = nearTerrain->Owner->GetComponent<Mesh>();
                    mesh && mesh->IsReady()) continue;
            bool covered = false;
            for (const FarTile& tile : m_farTiles)
            {
                if (!tile.object || !tile.object->enabled || !tile.mesh ||
                    !tile.mesh->IsReady() || !tile.hasGeometry ||
                    x < tile.startX || x >= tile.endX ||
                    y < tile.startY || y >= tile.endY) continue;
                if (tile.lodStride > 1)
                { covered = true; break; }
                const size_t index = static_cast<size_t>(y - tile.startY) *
                    (tile.endX - tile.startX) + x - tile.startX;
                if (index < tile.builtVisibility.size() &&
                    tile.builtVisibility[index])
                { covered = true; break; }
            }
            if (!covered) ++gaps;
        }
    return gaps;
}

size_t TerrainManager::GetPreparingFarTileCount() const
{
    return std::count_if(m_farTiles.begin(), m_farTiles.end(),
        [](const FarTile& tile) { return tile.preparing; });
}

size_t TerrainManager::GetQueuedFarTileCount() const
{
    std::lock_guard<std::mutex> lock(m_farPrepareMutex);
    return m_farPrepareQueue.size();
}

size_t TerrainManager::GetPreparedFarTileCount() const
{
    std::lock_guard<std::mutex> lock(m_farPrepareMutex);
    return m_preparedFarTiles.size();
}

size_t TerrainManager::GetVisibleFarTileVertexCount() const
{
    size_t count = 0;
    for (const auto& tile : m_farTiles)
        if (tile.object && tile.object->enabled && tile.hasGeometry &&
            tile.mesh) count += tile.mesh->GetVertexCount();
    return count;
}

std::vector<TerrainManager::FarLevelUsage> TerrainManager::GetFarLevelUsage() const
{
    std::map<int, FarLevelUsage> levels;
    for (const auto& tile : m_farTiles)
    {
        auto& level = levels[tile.lodStride];
        level.stride = tile.lodStride;
        if (!tile.mesh || !tile.hasGeometry) continue;
        ++level.residentTiles;
        if (tile.object && tile.object->enabled) ++level.visibleTiles;
        level.cpuBytes += tile.mesh->GetCpuMeshMemoryBytes() +
            tile.mesh->GetUploadShadowMemoryBytes();
        level.gpuBytes += tile.mesh->GetGpuBufferMemoryBytes();
    }
    std::vector<FarLevelUsage> result;
    for (const auto& [stride, level] : levels) result.push_back(level);
    return result;
}

}
