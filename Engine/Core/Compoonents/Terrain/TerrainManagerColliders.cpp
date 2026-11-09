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
void TerrainManager::ConfigureCellPhysics(Engine::Core::Object& object) const
{
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
