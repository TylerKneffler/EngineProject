#include <cstdlib>
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

const std::set<TerrainManager::Cell>& TerrainManager::VolumeEditOwners() const
{
    if (m_volumeEditOwners) return *m_volumeEditOwners;
    m_volumeEditOwners.emplace();
    std::error_code error;
    if (cellDataDirectory.empty() || !std::filesystem::is_directory(cellDataDirectory, error))
        return *m_volumeEditOwners;
    for (const auto& entry : std::filesystem::directory_iterator(cellDataDirectory, error))
    {
        if (entry.path().extension() != ".volumeedit") continue;
        int x = 0, z = 0;
        if (std::sscanf(entry.path().stem().string().c_str(), "%d_%d", &x, &z) == 2)
            m_volumeEditOwners->insert({x, z});
    }
    return *m_volumeEditOwners;
}

std::vector<TerrainManager::FarVolumeCell> TerrainManager::FarVolumeCells(
    int startX, int startY, int endX, int endY) const
{
    std::vector<FarVolumeCell> cells;
    const auto& owners = VolumeEditOwners();
    if (owners.empty()) return cells;
    const float spanX = cellsX * cellWidth, spanZ = cellsY * cellDepth;
    std::set<Cell> candidates;
    for (const Cell owner : owners)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const Cell cell{owner.first + dx, owner.second + dz};
                if (cell.first >= startX && cell.first < endX &&
                    cell.second >= startY && cell.second < endY) candidates.insert(cell);
            }
    for (const Cell cell : candidates)
    {
        FarVolumeCell reached{cell, {}};
        const auto append = [&](int dx, int dz) {
            const glm::vec3 offset(dx * spanX, 0.f, dz * spanZ);
            for (const auto& edit : GetVolumeEdits({cell.first + dx, cell.second + dz}))
            {
                const auto shifted = (dx || dz) ? edit.Shifted(offset) : edit;
                const glm::vec3 low = shifted.Minimum(), high = shifted.Maximum();
                if (high.x < 0.f || high.z < 0.f || low.x > spanX || low.z > spanZ) continue;
                reached.edits.push_back(shifted);
            }
        };
        append(0, 0);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dz) append(dx, dz);
        if (!reached.edits.empty()) cells.push_back(std::move(reached));
    }
    return cells;
}

void TerrainManager::FeedVolumeNeighbours(Cell cell, VolumeTerrain& volume) const
{
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    volume.edits = GetVolumeEdits(cell);
    volume.surfacePainter = m_surfacePainter ? m_surfacePainter(cell) : nullptr;
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

void TerrainManager::RefreshVolumeCells(Cell owner, const glm::vec3& low, const glm::vec3& high)
{
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    InvalidateTerrainField(owner);
    // The loaded volume cells it reaches (their margins included).
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
}

bool TerrainManager::SetVolumeEdits(Cell cell, std::vector<TerrainVolumeEdit> edits)
{
    const auto path = GetVolumeEditFilePath(cell);
    if (path.empty()) return false;
    std::error_code error;
    glm::vec3 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    const glm::vec3 origin(float(cell.first * spanX), 0.f, float(cell.second * spanZ));
    for (const std::vector<TerrainVolumeEdit>* list : {&GetVolumeEdits(cell),
             static_cast<const std::vector<TerrainVolumeEdit>*>(&edits)})
        for (const auto& edit : *list)
        {
            low = glm::min(low, edit.Minimum() + origin);
            high = glm::max(high, edit.Maximum() + origin);
        }
    if (edits.empty())
        std::filesystem::remove(path, error);
    else
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (!SaveTerrainVolumeEdits(path, edits)) return false;
    }
    if (m_volumeEditOwners)
    {
        if (edits.empty()) m_volumeEditOwners->erase(cell);
        else m_volumeEditOwners->insert(cell);
    }
    m_volumeEdits[cell] = std::move(edits);
    if (low.x <= high.x) RefreshVolumeCells(cell, low, high);
    else InvalidateTerrainField(cell);
    return true;
}

size_t TerrainManager::FollowVolumeEdit(const glm::vec3& low, const glm::vec3& high)
{
    const auto field = GetTerrainField();
    if (!field) return 0;
    const double spanX = static_cast<double>(cellsX) * cellWidth;
    const double spanZ = static_cast<double>(cellsY) * cellDepth;
    size_t changed = 0;
    const int x0 = int(std::floor(low.x / spanX)), x1 = int(std::floor(high.x / spanX));
    const int z0 = int(std::floor(low.z / spanZ)), z1 = int(std::floor(high.z / spanZ));
    for (int cz = z0; cz <= z1; ++cz)
        for (int cx = x0; cx <= x1; ++cx)
        {
            const Terrain* terrain = GetLoadedCell(cx, cz);
            if (!terrain || !terrain->Owner) continue;
            const glm::vec3 cellOrigin(float(cx * spanX), 0.f, float(cz * spanZ));
            for (auto* child : terrain->Owner->Children)
            {
                auto* layer = child ? child->GetComponent<TerrainObjectLayer>() : nullptr;
                if (!layer) continue;
                const glm::mat4 layerToCell = child->transform.GetLocalMatrix();
                const glm::mat4 cellToLayer = glm::inverse(layerToCell);
                bool layerChanged = false;
                for (size_t i = 0; i < layer->placements.size();)
                {
                    auto& placement = layer->placements[i];
                    const glm::vec3 local = cellOrigin +
                        glm::vec3(layerToCell * glm::vec4(placement.position, 1.f));
                    if (local.x < low.x || local.x > high.x || local.z < low.z || local.z > high.z ||
                        local.y < low.y - 2.f || local.y > high.y + 2.f) { ++i; continue; }
                    const float density = field->Density(local);
                    if (std::abs(density) < .3f) { ++i; continue; }  // still on the surface
                    // Buried: the top surface. In the air: the first ground below.
                    const auto surface = density > 0.f ? field->SurfaceAt(local.x, local.z)
                        : field->SurfaceAt(local.x, local.z, local.y);
                    if (!surface || surface->normal.y < .5f)
                    {
                        layer->placements.erase(layer->placements.begin() + std::ptrdiff_t(i));
                        layerChanged = true;
                        ++changed;
                        continue;
                    }
                    const glm::vec3 moved(local.x, surface->height, local.z);
                    placement.position = glm::vec3(cellToLayer * glm::vec4(moved - cellOrigin, 1.f));
                    layerChanged = true;
                    ++changed;
                    ++i;
                }
                if (layerChanged) layer->MarkConfigurationDirty();
            }
        }
    return changed;
}

bool TerrainManager::AddVolumeEdit(const TerrainVolumeEdit& edit, bool followObjects)
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
    if (m_volumeEditOwners) m_volumeEditOwners->insert(owner);
    RefreshVolumeCells(owner, low, high);
    if (followObjects) FollowVolumeEdit(low, high);
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
            // Nothing else draws a loaded cell (far tiles leave it out), so
            // its volume draws at any distance, coarsest level past lod3: at
            // 600 m the loaded area's far corners (up to ~720 m from a
            // viewer at its cell's edge) were holes.
            volume->drawDistance = std::numeric_limits<float>::max();
            if (const char* forced = std::getenv("ENGINE_VOLUME_DRAW_DISTANCE"))
                volume->drawDistance = static_cast<float>(std::atof(forced));
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
