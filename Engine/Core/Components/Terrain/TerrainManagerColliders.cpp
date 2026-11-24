#include "TerrainManager.h"
#include "TerrainObjectLayer.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/VolumeTerrain.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Inspection/IInspectorUi.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace Engine::Components
{
bool TerrainManager::NeighbourHeights(Cell cell, std::vector<float>& heights) const
{
    const size_t expected = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    if (const auto* loaded = GetLoadedCell(cell.first, cell.second);
        loaded && loaded->heights.size() == expected)
    {
        heights = loaded->heights;
        return true;
    }
    heights.clear();
    const auto* scene = Owner ? Owner->GetScene() : nullptr;
    bool read = false;
    if (scene && scene->HasStarted() && requireRuntimeBake)
        read = ReadBakedCellHeights(cell, heights);
    else if (!m_farHeightArchivePath.empty())
        read = TerrainHeightArchive::ReadCell(m_farHeightArchivePath, m_farSourceSignature,
            m_farHalfExtentCells, cellsX, cellsY, cell.first, cell.second, heights);
    return read && heights.size() == expected;
}

std::string TerrainManager::GetVolumeEditFilePath(Cell cell) const
{
    return cellDataDirectory.empty() ? std::string{} :
        (std::filesystem::path(cellDataDirectory) /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) + ".volumeedit")).string();
}

const std::vector<TerrainVolumeEdit>& TerrainManager::GetVolumeEdits(Cell cell) const
{
    auto [it, inserted] = m_volumeEdits.try_emplace(cell);
    if (inserted)
        if (const auto path = GetVolumeEditFilePath(cell); !path.empty() &&
            !LoadTerrainVolumeEdits(path, it->second))
            std::fprintf(stderr, "[TerrainManager] malformed volume edits: %s\n", path.c_str());
    return it->second;
}

void TerrainManager::FeedVolumeNeighbours(Cell cell, VolumeTerrain& volume) const
{
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    volume.edits = GetVolumeEdits(cell);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            auto& heights = volume.neighbourHeights[size_t((dz + 1) * 3 + dx + 1)];
            heights.clear();
            if (!dx && !dz) continue;
            const Cell neighbour{cell.first + dx, cell.second + dz};
            NeighbourHeights(neighbour, heights);
            const glm::vec3 offset(float(dx * spanX), 0.f, float(dz * spanZ));
            for (const auto& edit : GetVolumeEdits(neighbour))
                volume.edits.push_back(edit.Shifted(offset));
        }
}

bool TerrainManager::AddVolumeEdit(const TerrainVolumeEdit& edit)
{
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    if (spanX <= 0 || spanZ <= 0) return false;
    const glm::vec3 low = edit.Minimum(), high = edit.Maximum();
    // Only the owner's neighbours read it, so it must not reach past them.
    if (high.x - low.x >= spanX || high.z - low.z >= spanZ) return false;
    const glm::vec3 centre = (low + high) * .5f;
    const Cell owner{static_cast<int>(std::floor(centre.x / spanX)),
        static_cast<int>(std::floor(centre.z / spanZ))};
    const glm::vec3 origin(float(owner.first * spanX), 0.f, float(owner.second * spanZ));
    auto edits = GetVolumeEdits(owner);
    edits.push_back(edit.Shifted(-origin));
    const auto path = GetVolumeEditFilePath(owner);
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (!SaveTerrainVolumeEdits(path, edits)) return false;
    m_volumeEdits[owner] = std::move(edits);
    // Rebuild the loaded volume cells it reaches (their margins included).
    const float margin = 9.f * std::max(volumeSampleSpacing, .25f);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const Cell cell{owner.first + dx, owner.second + dz};
            const float x0 = float(cell.first * spanX) - margin, x1 = float((cell.first + 1) * spanX) + margin;
            const float z0 = float(cell.second * spanZ) - margin, z1 = float((cell.second + 1) * spanZ) + margin;
            if (high.x < x0 || low.x > x1 || high.z < z0 || low.z > z1) continue;
            const auto* terrain = GetLoadedCell(cell.first, cell.second);
            auto* volume = terrain && terrain->Owner ? terrain->Owner->GetComponent<VolumeTerrain>() : nullptr;
            if (!volume || !volume->fromTerrain) continue;
            FeedVolumeNeighbours(cell, *volume);
            volume->Refresh();
        }
    return true;
}

void TerrainManager::ConfigureCellPhysics(Engine::Core::Object& object) const
{
    // Volume cells: a finished cell (heights, stitched borders) is
    // drawn and collided by a volume built from it; no grid collider.
    const auto* cellTerrain = object.GetComponent<Terrain>();
    if (UsesVolumeCells() && cellTerrain)
    {
        auto* volume = object.GetComponent<VolumeTerrain>();
        if (!volume)
        {
            volume = object.AddComponent<VolumeTerrain>();
            volume->fromTerrain = true;
            volume->steepLayer = volumeSteepLayer;
            volume->sampleSpacing = volumeSampleSpacing;
            const double spanX = static_cast<double>(cellsX) * cellWidth;
            const double spanZ = static_cast<double>(cellsY) * cellDepth;
            const glm::dvec3 origin = object.transform.position;
            const Cell cell{static_cast<int>(std::lround(origin.x / spanX)),
                static_cast<int>(std::lround(origin.z / spanZ))};
            // Cells of this grid join into one sheet (VolumeTerrain seams).
            volume->group = this;
            volume->cellX = cell.first;
            volume->cellZ = cell.second;
            FeedVolumeNeighbours(cell, *volume);
            if (!generateColliders) volume->collisionDistance = 0.f;
            volume->Rebuild();
        }
        return;
    }
    if (!generateColliders) return;
    auto* mesh = object.GetComponent<Mesh>();
    if (!mesh || mesh->GetVertexCount() == 0u) return;
    auto* collider = object.GetComponent<MeshObjectCollider>();
    if (!collider) collider = object.AddComponent<MeshObjectCollider>();
    collider->convex = false;
    auto* body = object.GetComponent<RigidBody>();
    if (!body) body = object.AddComponent<RigidBody>();
    body->bodyType = "Static";
    body->useGravity = false;
    body->friction = 0.9f;
    body->restitution = 0.f;
}

void TerrainManager::RefreshAuthoredCollisionCells()
{
    if (!Owner || !Owner->GetScene()) return;
    const auto revision = Owner->GetScene()->GetStructureRevision();
    if (m_authoredCollisionRevision == revision) return;
    m_authoredCollisionCells.clear();
    const double width = static_cast<double>(cellsX) * cellWidth;
    const double depth = static_cast<double>(cellsY) * cellDepth;
    if (width <= 0 || depth <= 0) return;
    const glm::dmat4 inverse = glm::inverse(Owner->transform.GetWorldMatrixD());
    std::vector<Engine::Core::Object*> pending(Owner->Children.begin(), Owner->Children.end());
    while (!pending.empty())
    {
        auto* object = pending.back();
        pending.pop_back();
        if (!object || !object->IsEnabledInHierarchy()) continue;
        if (auto* terrain = object->GetComponent<Terrain>())
        {
            // Visual fallback terrain has no physics body and is not a cell
            // whose collider should be managed by this grid.
            if (!object->GetComponent<MeshObjectCollider>() ||
                !object->GetComponent<RigidBody>()) continue;
            const auto local = glm::dvec3(inverse * glm::dvec4(object->transform.GetWorldPositionD(), 1.0));
            const Cell cell{static_cast<int>(std::llround(local.x / width)),
                static_cast<int>(std::llround(local.z / depth))};
            m_authoredCollisionCells[cell] = terrain;
            // Grass descendants are irrelevant to terrain collision discovery.
            continue;
        }
        pending.insert(pending.end(), object->Children.begin(), object->Children.end());
    }
    m_authoredCollisionRevision = revision;
}

void TerrainManager::UpdateColliderStreamingAtPosition(const glm::dvec3& worldPosition)
{
    if (!Owner) return;
    const auto local = glm::dvec3(glm::inverse(Owner->transform.GetWorldMatrixD()) * glm::dvec4(worldPosition, 1.0));
    const double width = static_cast<double>(cellsX) * cellWidth;
    const double depth = static_cast<double>(cellsY) * cellDepth;
    if (width <= 0 || depth <= 0) return;
    m_collisionViewerLocal = local;
    const int centerX = static_cast<int>(std::floor(local.x / width));
    const int centerY = static_cast<int>(std::floor(local.z / depth));
    if (manageAuthoredCellColliders)
    {
        StreamPrefabCells(centerX, centerY);
        RefreshAuthoredCollisionCells();
    }
    RefreshColliderActivation(centerX, centerY);
}

void TerrainManager::StreamPrefabCells(int centerX, int centerY, bool unlimited)
{
    if (!Owner || !Owner->GetScene() || streamedPrefabPath.empty() ||
        streamedPrefabHalfExtent <= 0) return;
    auto& scene = *Owner->GetScene();
    const int radius = std::clamp(streamedPrefabRadius, 0, 8);
    std::vector<Engine::Core::Object*> reusable;
    for (auto it = m_streamedPrefabCells.begin(); it != m_streamedPrefabCells.end(); )
    {
        const Cell cell = it->first;
        auto* object = it->second;
        if (std::abs(cell.first - centerX) <= radius &&
            std::abs(cell.second - centerY) <= radius) { ++it; continue; }
        it = m_streamedPrefabCells.erase(it);
        if (reuseStreamedPrefabCells) reusable.push_back(object);
        else scene.RemoveObject(object);
    }
    bool changed = !reusable.empty();
    std::vector<Cell> candidates;
    for (int y = std::max(-streamedPrefabHalfExtent, centerY - radius);
        y <= std::min(streamedPrefabHalfExtent, centerY + radius); ++y)
        for (int x = std::max(-streamedPrefabHalfExtent, centerX - radius);
            x <= std::min(streamedPrefabHalfExtent, centerX + radius); ++x)
            if ((x || y) && !m_streamedPrefabCells.count({x, y}))
                candidates.emplace_back(x, y);
    std::sort(candidates.begin(), candidates.end(), [centerX, centerY](Cell a, Cell b) {
        const int da = (a.first - centerX) * (a.first - centerX) +
            (a.second - centerY) * (a.second - centerY);
        const int db = (b.first - centerX) * (b.first - centerX) +
            (b.second - centerY) * (b.second - centerY);
        return da == db ? a < b : da < db;
    });
    int budget = unlimited ? static_cast<int>(candidates.size()) : maxLoadsPerUpdate;
    for (const Cell cell : candidates)
    {
        Engine::Core::Object* object = nullptr;
        bool created = false;
        if (reuseStreamedPrefabCells && !reusable.empty())
        {
            object = reusable.back();
            reusable.pop_back();
        }
        else if (reuseStreamedPrefabCells && !m_streamedPrefabPool.empty())
        {
            object = m_streamedPrefabPool.back();
            m_streamedPrefabPool.pop_back();
        }
        else
        {
            if (budget-- <= 0) break;
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                scene, streamedPrefabPath, m_provider);
            if (!object) break;
            scene.SetParentLocal(object, Owner);
            object->runtimeOnly = true;
            created = true;
        }
        object->enabled = true;
        object->transform.position = {
            cell.first * cellsX * cellWidth, 0.f,
            cell.second * cellsY * cellDepth};
        object->name = streamedPrefabCellName + " (" + std::to_string(cell.first) +
            ", " + std::to_string(cell.second) + ")";
        m_streamedPrefabCells[cell] = object;
        changed = true;
        if (created && (scene.HasStarted() || m_startingPrefabCells)) object->Start();
    }
    for (auto* object : reusable)
    {
        object->enabled = false;
        object->name = "Pooled " + streamedPrefabCellName;
        if (auto* body = object->GetComponent<RigidBody>())
            body->SetCollisionSuspended(true);
        m_streamedPrefabPool.push_back(object);
    }
    if (changed) m_authoredCollisionRevision = 0;
}

void TerrainManager::RefreshColliderActivation(int centerX, int centerY)
{
    m_activeColliderCells = 0;
    if (!generateColliders) return;
    const double width = static_cast<double>(cellsX) * cellWidth;
    const double depth = static_cast<double>(cellsY) * cellDepth;
    if (width <= 0 || depth <= 0) return;
    double x = width * .5, z = depth * .5;
    if (m_collisionViewerLocal &&
        static_cast<int>(std::floor(m_collisionViewerLocal->x / width)) == centerX &&
        static_cast<int>(std::floor(m_collisionViewerLocal->z / depth)) == centerY)
    {
        x = m_collisionViewerLocal->x - centerX * width;
        z = m_collisionViewerLocal->z - centerY * depth;
    }
    const auto gridWorld = Owner ? Owner->transform.GetWorldMatrixD() : glm::dmat4(1.0);
    const auto neighbor = [this](double coordinate, double span, double worldScale) {
        const double margin = std::clamp(static_cast<double>(colliderActivationDistance) /
            std::max(worldScale, 1.e-9), 0.0, span * .499);
        return coordinate <= margin ? -1 : coordinate >= span - margin ? 1 : 0;
    };
    const int adjacentX = neighbor(x, width, glm::length(glm::dvec3(gridWorld[0])));
    const int adjacentZ = neighbor(z, depth, glm::length(glm::dvec3(gridWorld[2])));
    const auto& cells = manageAuthoredCellColliders ? m_authoredCollisionCells : m_loaded;
    for (const auto& [cell, terrain] : cells)
    {
        if (!terrain || !terrain->Owner) continue;
        auto* collider = terrain->Owner->GetComponent<MeshObjectCollider>();
        auto* body = terrain->Owner->GetComponent<RigidBody>();
        if (!collider || !body) continue;
        const bool active = !streamCollidersByDistance ||
            ((cell.first == centerX || (adjacentX && cell.first == centerX + adjacentX)) &&
             (cell.second == centerY || (adjacentZ && cell.second == centerY + adjacentZ)));
        // Retain rendering and scripts while removing distant native bodies.
        // Suspended bodies are skipped before physics configuration checks.
        body->SetCollisionSuspended(!active);
        if (collider->collisionEnabled != active)
        {
            collider->collisionEnabled = active;
            collider->MarkConfigurationDirty();
        }
        if (active) ++m_activeColliderCells;
    }
}

}
