#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace Engine::Components
{
namespace
{
int StreamingRequestScore(int dx, int dy, int travelX, int travelY)
{
    return 4 * (dx * dx + dy * dy) -
        2 * (dx * std::clamp(travelX, -1, 1) +
            dy * std::clamp(travelY, -1, 1));
}
}

void TerrainManager::UpdateStreaming(int centerX, int centerY)
{
    m_collisionViewerLocal.reset();
    if (manageAuthoredCellColliders)
    { StreamPrefabCells(centerX, centerY); RefreshAuthoredCollisionCells(); RefreshColliderActivation(centerX, centerY); return; }
    if (m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = false;
    m_prefetchCenter.reset();
    StreamCells(centerX, centerY);
}

void TerrainManager::UpdateStreamingAtPosition(
    const glm::dvec3& worldPosition)
{
    if (!Owner) return;
    if (manageAuthoredCellColliders)
    { UpdateColliderStreamingAtPosition(worldPosition); return; }
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanY = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanY <= 0.0) return;
    m_collisionViewerLocal = local;
    const double cellX = local.x / spanX;
    const double cellY = local.z / spanY;
    const int centerX = static_cast<int>(std::floor(cellX));
    const int centerY = static_cast<int>(std::floor(cellY));
    const double fractionX = cellX - centerX;
    const double fractionY = cellY - centerY;
    const int aheadX = fractionX >= .875 ? 1 : fractionX < .125 ? -1 : 0;
    const int aheadY = fractionY >= .875 ? 1 : fractionY < .125 ? -1 : 0;
    m_prefetchCenter = asyncStreaming && prefetchAdjacentCells && !m_editorStreaming &&
        (aheadX || aheadY)
        ? std::optional<Cell>{{centerX + aheadX, centerY + aheadY}}
        : std::nullopt;
    if (m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = false;
    StreamCells(centerX, centerY);
}

void TerrainManager::UpdateEditorStreaming(const glm::dvec3& worldPosition,
    const Camera* camera, int viewportHeight, float aspect)
{
    if (!Owner || !Owner->GetScene() || Owner->GetScene()->HasStarted()) return;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanZ <= 0.0) return;
    if (!m_editorStreaming) InvalidatePrepared();
    m_editorStreaming = true;
    m_prefetchCenter.reset();
    const int centerX = static_cast<int>(std::floor(local.x / spanX));
    const int centerY = static_cast<int>(std::floor(local.z / spanZ));
    if (manageAuthoredCellColliders)
    {
        m_collisionViewerLocal = local;
        StreamPrefabCells(centerX, centerY);
        RefreshAuthoredCollisionCells();
        RefreshColliderActivation(centerX, centerY);
        return;
    }
    StreamCells(centerX, centerY);
    if (m_farEnabled) UpdateFarTerrain(centerX, centerY, camera,
        viewportHeight, aspect);
}

bool TerrainManager::TryGetHeightAtWorldPosition(
    const glm::dvec3& worldPosition, double& height) const
{
    if (!Owner || cellsX <= 0 || cellsY <= 0 ||
        cellWidth <= 0.f || cellDepth <= 0.f)
        return false;
    const glm::dvec3 local = glm::dvec3(
        glm::inverse(Owner->transform.GetWorldMatrixD()) *
        glm::dvec4(worldPosition, 1.0));
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0.0 || spanZ <= 0.0)
        return false;
    const int cellX = static_cast<int>(std::floor(local.x / spanX));
    const int cellY = static_cast<int>(std::floor(local.z / spanZ));
    const Terrain* terrain = GetLoadedCell(cellX, cellY);
    if (!terrain || terrain->heights.size() !=
        static_cast<size_t>(cellsX + 1) * (cellsY + 1))
        return false;
    const double cellLocalX = local.x - static_cast<double>(cellX) * spanX;
    const double cellLocalZ = local.z - static_cast<double>(cellY) * spanZ;
    const double sampleX = std::clamp(cellLocalX /
        static_cast<double>(cellWidth), 0.0, static_cast<double>(cellsX));
    const double sampleZ = std::clamp(cellLocalZ /
        static_cast<double>(cellDepth), 0.0, static_cast<double>(cellsY));
    const int x0 = std::clamp(static_cast<int>(std::floor(sampleX)), 0, cellsX);
    const int z0 = std::clamp(static_cast<int>(std::floor(sampleZ)), 0, cellsY);
    const int x1 = std::min(cellsX, x0 + 1);
    const int z1 = std::min(cellsY, z0 + 1);
    const double tx = sampleX - static_cast<double>(x0);
    const double tz = sampleZ - static_cast<double>(z0);
    const auto sample = [&](int x, int z) {
        const size_t index = static_cast<size_t>(z) *
            static_cast<size_t>(cellsX + 1) + static_cast<size_t>(x);
        return static_cast<double>(terrain->heights[index]);
    };
    const double h00 = sample(x0, z0);
    const double h10 = sample(x1, z0);
    const double h01 = sample(x0, z1);
    const double h11 = sample(x1, z1);
    const double hx0 = h00 + (h10 - h00) * tx;
    const double hx1 = h01 + (h11 - h01) * tx;
    const double localHeight = hx0 + (hx1 - hx0) * tz;
    const glm::dvec4 world = Owner->transform.GetWorldMatrixD() *
        glm::dvec4(local.x, localHeight, local.z, 1.0);
    height = world.y;
    return std::isfinite(height);
}

void TerrainManager::StreamCells(int centerX, int centerY, int initialRadius)
{
    using ProfileClock = std::chrono::steady_clock;
    const auto profileStart = ProfileClock::now();
    auto phaseStart = profileStart;
    const auto record = [&](double& destination) {
        const auto now = ProfileClock::now();
        destination = std::chrono::duration<double, std::milli>(
            now - phaseStart).count();
        phaseStart = now;
    };
    m_lastStreamingTiming = {};
    const auto* scene = Owner ? Owner->GetScene() : nullptr;
    const bool runtimeBudget = scene && scene->HasStarted();
    if (runtimeBudget && m_uploadBudgetFrame != scene->GetFrameIndex())
    {
        m_uploadBudgetFrame = scene->GetFrameIndex();
        m_nearFrameUploads = 0;
        m_farFrameUploads = 0;
    }
    const int frameCapacity = std::clamp(maxUploadsPerUpdate, 1, 64);
    const size_t alreadyUploaded = m_nearFrameUploads + m_farFrameUploads;
    const int frameRemaining = runtimeBudget ? static_cast<int>(
        alreadyUploaded < static_cast<size_t>(frameCapacity)
            ? frameCapacity - alreadyUploaded : 0) : frameCapacity;
    const uint64_t batchesBefore = m_renderBatchUploadCount;
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    {
        Terrain* terrain = it->second;
        if (!terrain || !terrain->Owner || !terrain->Owner->GetScene())
        {
            it = m_loaded.erase(it);
            continue;
        }
        ++it;
    }
    const int radius = initialRadius >= 0 ? std::clamp(initialRadius, 0, 32) :
        m_editorStreaming ? std::clamp(editorRadius, 0, 32) :
        std::clamp(m_farEnabled && !m_farHeights.empty() ? loadedRadius :
            std::max(loadedRadius, farRadius), 0, 32);
    const auto farCovers = [&](Cell cell) {
        for (const auto& tile : m_farTiles)
        {
            if (!tile.object || !tile.object->enabled || !tile.hasGeometry ||
                cell.first < tile.startX || cell.first >= tile.endX ||
                cell.second < tile.startY || cell.second >= tile.endY)
                continue;
            if (tile.lodStride > 1) return true;
            const size_t index = static_cast<size_t>(cell.second - tile.startY) *
                (tile.endX - tile.startX) + cell.first - tile.startX;
            if (index < tile.builtVisibility.size() &&
                tile.builtVisibility[index]) return true;
        }
        return false;
    };
    const auto wanted = [&](Cell cell) {
        const auto inside = [&](Cell center) {
            return std::abs(cell.first - center.first) <= radius &&
                std::abs(cell.second - center.second) <= radius;
        };
        return inside({centerX, centerY}) ||
            (m_prefetchCenter && inside(*m_prefetchCenter));
    };
    const Cell travel{m_haveLastCenter ? centerX - m_lastCenter.first : 0,
        m_haveLastCenter ? centerY - m_lastCenter.second : 0};
    m_lastCenter = {centerX, centerY};
    m_haveLastCenter = true;
    record(m_lastStreamingTiming.housekeepingMs);
    if (asyncStreaming)
    {
        StartPrepareWorker();
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        for (auto it = m_prepareQueue.begin(); it != m_prepareQueue.end(); )
        {
            const Cell cell = it->cell;
            if (!wanted(cell) ||
                !HasCell(cell.first, cell.second))
            {
                m_preparingCells.erase(cell);
                it = m_prepareQueue.erase(it);
            }
            else
            {
                it->lodStride = DesiredPrepareLodStride(cell, centerX, centerY);
                ++it;
            }
        }
        const auto score = [&](Cell cell) {
            const int dx = cell.first - centerX, dy = cell.second - centerY;
            return StreamingRequestScore(dx, dy, travel.first, travel.second);
        };
        std::sort(m_prepareQueue.begin(), m_prepareQueue.end(),
            [&](const PrepareTask& a, const PrepareTask& b) {
                const int da = score(a.cell), db = score(b.cell);
                return da == db ? a.cell < b.cell : da < db;
            });
    }
    record(m_lastStreamingTiming.queueMs);
    int unloaded = 0;
    for (auto it = m_loaded.begin(); it != m_loaded.end() &&
        unloaded < std::clamp(maxUnloadsPerUpdate, 1, 64); )
    {
        const Cell cell = it->first;
        ++it;
        if ((std::abs(cell.first - centerX) > radius ||
            std::abs(cell.second - centerY) > radius) &&
            (!m_farEnabled || farCovers(cell)))
        {
            UnloadCell(cell);
            ++unloaded;
        }
    }
    record(m_lastStreamingTiming.unloadMs);
    std::vector<Cell> pending;
    const int minX = std::min(centerX,
        m_prefetchCenter ? m_prefetchCenter->first : centerX) - radius;
    const int maxX = std::max(centerX,
        m_prefetchCenter ? m_prefetchCenter->first : centerX) + radius;
    const int minY = std::min(centerY,
        m_prefetchCenter ? m_prefetchCenter->second : centerY) - radius;
    const int maxY = std::max(centerY,
        m_prefetchCenter ? m_prefetchCenter->second : centerY) + radius;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
            if (wanted({x, y}) && HasCell(x, y) && !GetLoadedCell(x, y) &&
                (!asyncStreaming || !m_preparingCells.count({x, y})))
                pending.emplace_back(x, y);
    const auto priority = [&](Cell cell) {
        const int dx = cell.first - centerX, dy = cell.second - centerY;
        return StreamingRequestScore(dx, dy, travel.first, travel.second);
    };
    std::sort(pending.begin(), pending.end(), [&](Cell a, Cell b) {
        const int da = priority(a), db = priority(b);
        return da == db ? a < b : da < db;
    });
    record(m_lastStreamingTiming.candidatesMs);
    int loaded = 0;
    // Settled windows have no load requests; avoid walking all resident
    // meshes and locking both worker queues for a budget we will not use.
    const auto beforeLoads = pending.empty() ? MemoryUsage{} : GetMemoryUsage();
    const bool memoryFull =
        beforeLoads.cpuBytes >= (static_cast<uint64_t>(std::max(1, cpuBudgetMiB)) << 20) ||
        beforeLoads.gpuBytes >= (static_cast<uint64_t>(std::max(1, gpuBudgetMiB)) << 20);
    for (Cell cell : pending)
    {
        if (loaded >= std::clamp(maxLoadsPerUpdate, 1, 64)) break;
        if (runtimeBudget && !asyncStreaming && loaded >= frameRemaining) break;
        if (memoryFull) break;
        if (asyncStreaming)
        {
            if (m_preparingCells.size() >=
                static_cast<size_t>(std::clamp(maxPreparedCells, 1, 64))) break;
            PrepareTask task;
            task.cell = cell;
            task.revision = m_prepareRevision;
            task.scene = Owner->GetScene();
            task.sceneGeneration = task.scene->CaptureSceneGeneration();
            task.cacheSignature = m_cacheSignature;
            task.cellsX = cellsX; task.cellsY = cellsY;
            task.proceduralOakMeadow = proceduralOakMeadow;
            task.cellWidth = cellWidth; task.cellDepth = cellDepth;
            task.lodStride = DesiredPrepareLodStride(cell, centerX, centerY);
            const auto saved = m_cells.find(cell);
            task.hasSaved = saved != m_cells.end();
            if (task.hasSaved) task.saved = saved->second;
            task.builder = m_proceduralBuilder;
            if (!task.hasSaved && !task.builder &&
                !(Owner->GetScene()->HasStarted() && requireRuntimeBake)) continue;
            task.cachePath = GetCacheFilePath(cell.first, cell.second);
            if ((m_runtimeBakeActive || requireRuntimeBake) &&
                Owner->GetScene()->HasStarted())
            {
                task.bakePath = GetRuntimeBakeArchivePath();
                if (const auto found = m_bakeIndex.find(cell);
                    found != m_bakeIndex.end())
                {
                    task.bakeOffset = found->second.offset;
                    task.bakeSize = found->second.size;
                }
            }
            task.requireBake = Owner->GetScene()->HasStarted() &&
                (m_runtimeBakeActive || requireRuntimeBake);
            task.edited = m_editedProcedural.count(cell) != 0;
            task.editPath = GetEditFilePath(cell);
            std::set<std::string> layerNames;
            if (const auto found = m_objectLayers.find(cell);
                found != m_objectLayers.end())
                for (size_t i = 0; i < found->second.ArraySize(); ++i)
                {
                    const auto& savedLayer = found->second.ArrayAt(i);
                    const auto name = savedLayer["layerName"].AsString();
                    if (!name.empty() && layerNames.insert(name).second)
                        task.objectLayers.emplace_back(name, savedLayer);
                }
            for (const auto& name : m_objectLayerNames)
                if (!name.empty() && layerNames.insert(name).second)
                    task.objectLayers.emplace_back(name, JsonValue{});
            {
                std::lock_guard<std::mutex> lock(m_prepareMutex);
                m_prepareQueue.push_back(std::move(task));
            }
            m_preparingCells[cell] = m_prepareRevision;
            m_prepareWake.notify_one();
            ++loaded;
        }
        else
        {
            LoadCell(cell, DesiredLodStride(cell, centerX, centerY));
            if (GetLoadedCell(cell.first, cell.second)) ++loaded;
        }
    }
    m_lastStreamingTiming.scheduledCells = loaded;
    record(m_lastStreamingTiming.loadOrScheduleMs);
    std::vector<Cell> lodPending;
    for (const auto& [cell, terrain] : m_loaded)
        if (terrain && terrain->Owner &&
            terrain->GetMeshLodStride() != DesiredLodStride(cell, centerX, centerY))
            lodPending.push_back(cell);
    std::sort(lodPending.begin(), lodPending.end(), [&](Cell a, Cell b) {
        const auto foundA = m_loaded.find(a);
        const auto foundB = m_loaded.find(b);
        if (foundA == m_loaded.end() || !foundA->second ||
            !foundA->second->Owner) return false;
        if (foundB == m_loaded.end() || !foundB->second ||
            !foundB->second->Owner) return true;
        const int strideA = DesiredLodStride(a, centerX, centerY);
        const int strideB = DesiredLodStride(b, centerX, centerY);
        const bool upgradeA = strideA < foundA->second->GetMeshLodStride();
        const bool upgradeB = strideB < foundB->second->GetMeshLodStride();
        if (upgradeA != upgradeB) return upgradeA;
        const int distanceA = std::max(std::abs(a.first - centerX), std::abs(a.second - centerY));
        const int distanceB = std::max(std::abs(b.first - centerX), std::abs(b.second - centerY));
        return distanceA == distanceB ? a < b : distanceA < distanceB;
    });
    int changed = 0;
    const int uploadCapacity = runtimeBudget
        ? std::max(0, frameRemaining - (asyncStreaming ? 0 : loaded))
        : std::clamp(maxUploadsPerUpdate, 1, 64);
    const int reservedBatchUploads =
        asyncStreaming && batchFarCells && m_renderBatchRefreshPending ? 1 : 0;
    for (Cell cell : lodPending)
    {
        const int lodLimit = runtimeBudget || asyncStreaming
            ? std::min(std::clamp(maxLodChangesPerUpdate, 1, 64),
                std::max(0, uploadCapacity - reservedBatchUploads))
            : std::clamp(maxLodChangesPerUpdate, 1, 64);
        if (changed >= lodLimit) break;
        if (auto found = m_loaded.find(cell);
            found != m_loaded.end() && found->second && found->second->Owner)
        {
            found->second->SetMeshLodStride(
                DesiredLodStride(cell, centerX, centerY));
            ++changed;
        }
    }
    m_lastStreamingTiming.changedLods = changed;
    record(m_lastStreamingTiming.lodMs);
    int batchUploads = runtimeBudget
        ? std::max(0, uploadCapacity - changed) : 8;
    int preparedUploads = 0;
    if (asyncStreaming)
    {
        if (!runtimeBudget)
            batchUploads = std::max(0, uploadCapacity - changed);
        const int preparedBudget = batchUploads > 0
            ? std::min(batchUploads, std::max(1,
                batchUploads - reservedBatchUploads))
            : 0;
        preparedUploads = CommitPrepared(centerX, centerY, radius,
            preparedBudget);
        batchUploads -= preparedUploads;
    }
    m_lastStreamingTiming.committedCells = preparedUploads;
    record(m_lastStreamingTiming.commitMs);
    RefreshColliderActivation(centerX, centerY);
    record(m_lastStreamingTiming.colliderActivationMs);
    RefreshRenderBatches(batchUploads, centerX, centerY, radius);
    record(m_lastStreamingTiming.batchRefreshMs);
    if (runtimeBudget)
        m_nearFrameUploads += changed + preparedUploads +
            (asyncStreaming ? 0 : loaded) +
            static_cast<size_t>(m_renderBatchUploadCount - batchesBefore);
    TrimMemoryBudget(centerX, centerY);
    record(m_lastStreamingTiming.memoryTrimMs);
    m_lastStreamingTiming.totalMs =
        std::chrono::duration<double, std::milli>(
            phaseStart - profileStart).count();
}

void TerrainManager::Update()
{
    if (!Owner || viewerObjectName.empty()) return;
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer) return;
    if (manageAuthoredCellColliders)
        UpdateColliderStreamingAtPosition(viewer->transform.GetWorldPositionD());
    else UpdateStreamingAtPosition(viewer->transform.GetWorldPositionD());
}

void TerrainManager::Start()
{
    if (manageAuthoredCellColliders)
    {
        m_authoredCollisionRevision = 0;
        RefreshAuthoredCollisionCells();
        m_startingPrefabCells = true;
        Update();
        m_startingPrefabCells = false;
        return;
    }
    m_runtimeBakeActive = false;
    m_bakeIndex.clear();
    if (runtimeBakeDirectory.empty())
    {
        m_bakeStatus = requireRuntimeBake ?
            "Runtime bake required; set a bake directory and bake terrain" :
            "No runtime bake directory; using source terrain";
    }
    else
    {
        m_runtimeBakeActive = ReadRuntimeBakeIndex(
            GetRuntimeBakeArchivePath(), m_bakeIndex);
        if (!m_runtimeBakeActive) m_bakeIndex.clear();
        m_bakeStatus = m_runtimeBakeActive ? "Runtime terrain bake active" :
            requireRuntimeBake ? "Runtime bake required; use Bake all terrain cells" :
            "No complete runtime bake; using source terrain";
    }
    InvalidatePrepared();
    for (auto it = m_loaded.begin(); it != m_loaded.end(); )
    {
        const Cell cell = it->first;
        ++it;
        UnloadCell(cell);
    }
    for (auto& [group, batch] : m_renderBatches)
        if (Owner && Owner->GetScene()) Owner->GetScene()->RemoveObject(batch.object);
    m_renderBatches.clear();
    m_haveLastCenter = false;
}

}
