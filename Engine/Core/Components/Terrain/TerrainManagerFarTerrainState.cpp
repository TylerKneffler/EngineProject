#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/VolumeTerrain.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
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
    // Edited cells' heights (the archive does not have them).
    auto patchHeights = std::make_shared<std::map<Cell, std::vector<float>>>();
    for (auto& patch : patches) (*patchHeights)[{patch.x, patch.y}] = std::move(patch.heights);
    m_farPatchHeights = std::move(patchHeights);
    m_farHeightArchivePath = std::move(heightArchivePath);
    m_farSourceSignature = sourceSignature;
    m_farPalette = std::move(palette);
    m_farPaletteShared = std::make_shared<const FarTerrainPalette>(m_farPalette);
    m_farEnabled = true;
    m_farDirty = true;
    // Far nodes finer than a cell read the archive's full detail: load it
    // into memory off the main thread.
    if (!m_farHeightArchivePath.empty())
        m_farArchiveJob = std::async(std::launch::async,
            [path = m_farHeightArchivePath, signature = m_farSourceSignature,
                extent = m_farHalfExtentCells, x = cellsX, y = cellsY]()
                -> std::shared_ptr<const FarCellDetail::Archive> {
                auto archive = std::make_shared<FarCellDetail::Archive>();
                if (!archive->Load(path, signature, extent, x, y)) return nullptr;
                return archive;
            });
    // Volume cells built before the archive was known repeated their border
    // where an unloaded neighbour's heights were missing: build them again.
    for (const auto& [cell, terrain] : m_loaded)
    {
        auto* volume = terrain && terrain->Owner ? terrain->Owner->GetComponent<VolumeTerrain>() : nullptr;
        if (!volume || !volume->fromTerrain) continue;
        bool missing = false;
        for (size_t i = 0; i < volume->neighbourHeights.size(); ++i)
            missing = missing || (i != 4 && volume->neighbourHeights[i].empty());
        if (!missing) continue;
        FeedVolumeNeighbours(cell, *volume);
        volume->Refresh();
    }
}

void TerrainManager::RefreshFarTerrainEditedCells(const std::set<Cell>& changedCells)
{
    if (m_farHeights.empty() || changedCells.empty()) return;
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    auto patches = m_farPatchHeights
        ? std::make_shared<std::map<Cell, std::vector<float>>>(*m_farPatchHeights)
        : std::make_shared<std::map<Cell, std::vector<float>>>();
    const int side = m_farHalfExtentCells * 2 + 2;
    for (Cell cell : changedCells)
    {
        if (std::abs(cell.first) > m_farHalfExtentCells ||
            std::abs(cell.second) > m_farHalfExtentCells) continue;
        const Terrain* terrain = GetLoadedCell(cell.first, cell.second);
        if (!terrain || terrain->heights.size() != expected) continue;
        (*patches)[cell] = terrain->heights;
        // The corner grid follows the cell's corners.
        const auto setCorner = [&](int x, int y, float height) {
            const int gx = x + m_farHalfExtentCells, gy = y + m_farHalfExtentCells;
            if (gx >= 0 && gy >= 0 && gx < side && gy < side)
                m_farHeights[static_cast<size_t>(gy) * side + gx] = height;
        };
        setCorner(cell.first, cell.second, terrain->heights.front());
        setCorner(cell.first + 1, cell.second, terrain->heights[static_cast<size_t>(cellsX)]);
        setCorner(cell.first, cell.second + 1, terrain->heights[static_cast<size_t>(cellsY) * (cellsX + 1)]);
        setCorner(cell.first + 1, cell.second + 1, terrain->heights.back());
        MarkFarVolumeDirty(cell);
    }
    m_farPatchHeights = std::move(patches);
    m_farHeightsSnapshot = std::make_shared<const std::vector<float>>(m_farHeights);
}

void TerrainManager::ClearFarTerrain()
{
    ClearFarObjects();
    FenceFarVolumeJobs();
    if (Owner && Owner->GetScene())
        for (const auto& [key, node] : m_farNodes)
            if (node.object) Owner->GetScene()->RemoveObject(node.object);
    m_farNodes.clear();
    m_farWanted.clear();
    m_farShown.clear();
    m_farBuildOrder.clear();
    m_farShownCells.clear();
    m_farStepValid = false;
    m_farDirty = false;
    m_farHeights.clear();
    m_farHeightsSnapshot.reset();
    ResetSunShadow();
    m_farPatchHeights.reset();
    m_farHeightArchivePath.clear();
    if (m_farArchiveJob.valid()) m_farArchiveJob.wait();
    m_farArchiveJob = {};
    m_farArchive.reset();
    m_farHalfExtentCells = 0;
    m_farSourceSignature = 0;
    m_farTileUploadCount = 0;
    m_farEnabled = false;
}

void TerrainManager::SetFarTerrainEnabled(bool enabled)
{
    if (m_farEnabled == enabled) return;
    m_farEnabled = enabled;
    if (!enabled)
        for (const auto& [key, node] : m_farNodes)
            if (node.object) node.object->enabled = false;
    if (enabled)
    {
        // Show the last set again.
        for (const auto& key : m_farShown)
            if (const auto found = m_farNodes.find(key); found != m_farNodes.end() && found->second.object)
                found->second.object->enabled = found->second.hasGeometry;
        m_farDirty = true;
    }
}

size_t TerrainManager::GetVisibleFarTileCountForStride(int stride) const
{
    size_t count = 0;
    for (const auto& key : m_farShown) count += key.size == stride;
    return count;
}
size_t TerrainManager::GetQueuedFarTileCount() const { return 0; }
size_t TerrainManager::GetPreparedFarTileCount() const { return 0; }
}
