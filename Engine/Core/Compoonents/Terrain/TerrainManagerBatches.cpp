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
void TerrainManager::RefreshRenderBatches(int uploadBudget, int centerX,
    int centerY, int radius)
{
    if (!Owner || !Owner->GetScene()) return;
    const auto refreshStart = std::chrono::steady_clock::now();
    m_renderBatchRefreshPending = false;
    constexpr int side = 4;
    const auto groupOf = [side](Cell cell) -> Cell {
        const auto floorDiv = [side](int value) {
            return value >= 0 ? value / side : (value - side + 1) / side;
        };
        return {floorDiv(cell.first), floorDiv(cell.second)};
    };
    using Members = std::vector<std::pair<Cell, Terrain*>>;
    std::map<Cell, Members> groups;
    if (batchFarCells)
    for (const auto& [cell, terrain] : m_loaded)
    {
        if (!terrain || !terrain->Owner || terrain->GetMeshLodStride() < 2)
            continue;
        auto* mesh = terrain->Owner->GetComponent<Mesh>();
        if (mesh && mesh->IsReady() && !mesh->GetVertices().empty() &&
            !mesh->GetIndices().empty())
            groups[groupOf(cell)].emplace_back(cell, terrain);
    }
    const auto revisionOf = [](const Members& members) {
        uint64_t revision = 1469598103934665603ull;
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh) continue;
            for (uint64_t value : {static_cast<uint64_t>(static_cast<uint32_t>(cell.first)),
                    static_cast<uint64_t>(static_cast<uint32_t>(cell.second)),
                    mesh->GetAuthoredGeometryRevision(),
                    terrain->Owner->transform.GetWorldRevision()})
                revision = (revision ^ value) * 1099511628211ull;
        }
        return revision;
    };
    for (auto it = m_renderBatches.begin(); it != m_renderBatches.end(); )
    {
        const auto found = groups.find(it->first);
        if (found != groups.end() && found->second.size() >= 2 &&
            it->second.revision == revisionOf(found->second))
        { ++it; continue; }
        // A far-cell LOD change uploads its cell mesh first. With no upload
        // slot left, keep drawing the previous batch for the same members;
        // replace it on a later frame instead of exposing every cell draw.
        if (!m_editorStreaming && uploadBudget <= 0 &&
            found != groups.end() &&
            found->second.size() == it->second.members.size())
        {
            bool sameMembers = true;
            bool lodChanged = false;
            for (const auto& [cell, terrain] : found->second)
            {
                if (!terrain || !terrain->Owner)
                { sameMembers = false; break; }
                const auto oldLod = it->second.lodStrides.find(cell);
                const auto oldGeometry =
                    it->second.geometryRevisions.find(cell);
                const auto oldTransform =
                    it->second.transformRevisions.find(cell);
                const auto* mesh = terrain->Owner->GetComponent<Mesh>();
                if (!it->second.members.count(cell) ||
                    oldLod == it->second.lodStrides.end() ||
                    oldGeometry == it->second.geometryRevisions.end() ||
                    oldTransform == it->second.transformRevisions.end() ||
                    oldTransform->second !=
                        terrain->Owner->transform.GetWorldRevision() ||
                    (!mesh || (oldLod->second == terrain->GetMeshLodStride() &&
                        oldGeometry->second != mesh->GetAuthoredGeometryRevision())))
                { sameMembers = false; break; }
                lodChanged |= oldLod->second != terrain->GetMeshLodStride();
            }
            if (sameMembers && lodChanged)
            {
                m_renderBatchRefreshPending = true;
                ++it;
                continue;
            }
        }
        // A moving streaming window fills a 4x4 group one cell at a time.
        // Keep its old batch while the missing cells render individually;
        // replace it once every cell required by this window has arrived.
        bool waitingForCells = false;
        for (int y = it->first.second * side;
            y < (it->first.second + 1) * side && !waitingForCells; ++y)
            for (int x = it->first.first * side;
                x < (it->first.first + 1) * side; ++x)
                if (std::abs(x - centerX) <= radius &&
                    std::abs(y - centerY) <= radius && HasCell(x, y) &&
                    !m_loaded.count({x, y}))
                { waitingForCells = true; break; }
        if (found != groups.end() && waitingForCells &&
            found->second.size() > it->second.members.size())
        {
            Members previousMembers;
            for (const auto& member : found->second)
                if (member.second && member.second->Owner &&
                    it->second.members.count(member.first))
                    previousMembers.push_back(member);
            if (previousMembers.size() == it->second.members.size() &&
                it->second.revision == revisionOf(previousMembers))
            { ++it; continue; }
        }
        for (Cell cell : it->second.members)
            if (auto* terrain = GetLoadedCell(cell.first, cell.second))
                if (auto* mesh = terrain->Owner->GetComponent<Mesh>())
                    mesh->SetEditorVisible(true);
        Owner->GetScene()->RemoveObject(it->second.object);
        it = m_renderBatches.erase(it);
    }
    for (const auto& [group, members] : groups)
    {
        if (members.size() < 2 || m_renderBatches.count(group)) continue;
        const Terrain* first = members.front().second;
        if (!first || !first->Owner) continue;
        const auto sameAppearance = [&](const Terrain* terrain) {
            if (!terrain || !terrain->Owner) return false;
            if (terrain->layers.size() != first->layers.size() ||
                terrain->textureTiling != first->textureTiling) return false;
            for (size_t layer = 0; layer < first->layers.size(); ++layer)
                if (terrain->layers[layer].material != first->layers[layer].material)
                    return false;
            return true;
        };
        if (!std::all_of(members.begin(), members.end(),
            [&](const auto& member) { return sameAppearance(member.second); }))
            continue;
        if (uploadBudget <= 0)
        {
            m_renderBatchRefreshPending = true;
            continue;
        }
        const auto buildStart = std::chrono::steady_clock::now();
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        size_t vertexCount = 0, indexCount = 0;
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh || mesh->GetVertices().empty() || mesh->GetIndices().empty())
                continue;
            vertexCount += mesh->GetVertices().size();
            indexCount += mesh->GetIndices().size();
        }
        if (vertexCount == 0 || indexCount == 0) continue;
        vertices.reserve(vertexCount);
        indices.reserve(indexCount);
        for (const auto& [cell, terrain] : members)
        {
            if (!terrain || !terrain->Owner) continue;
            const auto* mesh = terrain->Owner->GetComponent<Mesh>();
            if (!mesh || mesh->GetVertices().empty() || mesh->GetIndices().empty())
                continue;
            const glm::mat4 world = terrain->Owner->transform.GetWorldMatrix();
            const bool translationOnly =
                glm::length(glm::vec3(world[0]) - glm::vec3(1.f, 0.f, 0.f)) < 1e-6f &&
                glm::length(glm::vec3(world[1]) - glm::vec3(0.f, 1.f, 0.f)) < 1e-6f &&
                glm::length(glm::vec3(world[2]) - glm::vec3(0.f, 0.f, 1.f)) < 1e-6f;
            const glm::mat3 normalMatrix = translationOnly
                ? glm::mat3(1.f) :
                glm::transpose(glm::inverse(glm::mat3(world)));
            const glm::vec3 translation(world[3]);
            const uint32_t base = static_cast<uint32_t>(vertices.size());
            for (auto vertex : mesh->GetVertices())
            {
                const glm::vec3 position = translationOnly
                    ? glm::vec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]) + translation
                    : glm::vec3(world * glm::vec4(
                        vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
                vertex.pos[0] = position.x; vertex.pos[1] = position.y;
                vertex.pos[2] = position.z;
                if (!translationOnly)
                {
                    const glm::vec3 normal = glm::normalize(normalMatrix *
                        glm::vec3(vertex.normal[0], vertex.normal[1],
                            vertex.normal[2]));
                    vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                }
                vertices.push_back(vertex);
            }
            const size_t vertexSize = mesh->GetVertices().size();
            for (uint32_t index : mesh->GetIndices())
            {
                if (index >= vertexSize) continue;
                indices.push_back(base + index);
            }
        }
        if (vertices.empty() || indices.empty()) continue;
        auto* object = Owner->GetScene()->AddObject("Terrain batch " +
            std::to_string(group.first) + "," + std::to_string(group.second));
        object->runtimeOnly = true;
        Owner->GetScene()->MoveObject(object, Owner,
            Engine::Scene::Scene::ObjectPlacement::AsChild);
        auto* batchMesh = object->AddComponent<Mesh>();
        if (!batchMesh->SetIndexedGeometry(std::move(vertices), std::move(indices)))
        {
            Owner->GetScene()->RemoveObject(object);
            continue;
        }
        batchMesh->OnAfterDeserialize(m_provider);
        auto* appearance = object->AddComponent<TerrainBatchAppearance>();
        appearance->materials = first->GetLayerMaterials();
        appearance->textureTiling = first->textureTiling;
        appearance->completeFootprint = members.size() == side * side;
        RenderBatch batch;
        batch.object = object;
        batch.revision = revisionOf(members);
        for (const auto& [cell, terrain] : members)
        {
            batch.members.insert(cell);
            batch.lodStrides[cell] = terrain->GetMeshLodStride();
            batch.geometryRevisions[cell] = terrain->Owner->
                GetComponent<Mesh>()->GetAuthoredGeometryRevision();
            batch.transformRevisions[cell] =
                terrain->Owner->transform.GetWorldRevision();
            terrain->Owner->GetComponent<Mesh>()->SetEditorVisible(false);
        }
        m_renderBatches[group] = std::move(batch);
        ++m_renderBatchUploadCount;
        m_renderBatchBuildMilliseconds += std::chrono::duration<double,
            std::milli>(std::chrono::steady_clock::now() - buildStart).count();
        --uploadBudget;
    }
    m_renderBatchRefreshMilliseconds += std::chrono::duration<double,
        std::milli>(std::chrono::steady_clock::now() - refreshStart).count();
}

}
