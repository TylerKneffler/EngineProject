#include "Scripts/TerrainGen/TerrainGen.h"
#include "Scripts/TerrainGen/TerrainChunk.h"
#include "Scripts/TerrainGen/TerrainPatch.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
using Object = Engine::Core::Object;
using Scene = Engine::Scene::Scene;

Object* AddChild(Scene& scene, Object& parent, const std::string& name)
{
    Object* child = scene.AddObject(name);
    scene.SetParentLocal(child, &parent);
    return child;
}

template<typename T>
T* EnsureComponent(Object& object)
{
    if (T* existing = object.GetComponent<T>())
        return existing;
    return object.AddComponent<T>();
}
}
void TerrainGen::BuildChunk(int chunkX, int chunkZ,
    std::vector<CachedPatchMesh>* preparedPatches)
{
    if (!Owner || !Owner->GetScene())
        return;
    PerlinNoiseField* noise = ResolveNoise();
    if (!noise)
        return;

    const uint64_t configuration = MeshConfigurationHash(*noise);
    if (m_meshConfigurationHash != 0u &&
        m_meshConfigurationHash != configuration)
        m_meshCache.clear();
    m_meshConfigurationHash = configuration;

    Scene& scene = *Owner->GetScene();
    const int64_t key = ChunkKey(chunkX, chunkZ);
    auto cachedChunk = m_meshCache.find(key);
    const bool usesCachedMesh = !preparedPatches && cacheUnloadedChunkMeshes &&
        cachedChunk != m_meshCache.end();
    if (usesCachedMesh)
        ++m_meshCacheHits;
    else if (!preparedPatches)
        ++m_meshCacheMisses;
    Object* chunkObject = nullptr;
    const auto existingChunk = m_chunks.find(key);
    if (existingChunk != m_chunks.end())
        chunkObject = existingChunk->second;
    if (!chunkObject)
    {
        char name[64]{};
        std::snprintf(name, sizeof(name), "Chunk (%d, %d)", chunkX, chunkZ);
        if (!m_chunkObjectPool.empty())
        {
            chunkObject = m_chunkObjectPool.back();
            m_chunkObjectPool.pop_back();
            chunkObject->name = name;
            chunkObject->enabled = true;
            scene.SetParentLocal(chunkObject, Owner);
        }
        else
        {
            chunkObject = AddChild(scene, *Owner, name);
        }
        m_chunks[key] = chunkObject;
    }
    // Chunk coordinates are authoritative. Reapply the local origin even for
    // reused/editor-authored chunk objects so an old serialized transform can
    // never change the spacing between generated meshes.
    chunkObject->transform.position = {
        static_cast<float>((static_cast<int64_t>(chunkX) - worldOriginChunkX) *
            std::max(1.f, chunkSize)), 0.f,
        static_cast<float>((static_cast<int64_t>(chunkZ) - worldOriginChunkZ) *
            std::max(1.f, chunkSize)) };

    TerrainChunk* chunk = EnsureComponent<TerrainChunk>(*chunkObject);
    chunk->chunkX = chunkX;
    chunk->chunkZ = chunkZ;
    const int patchAxisCount = std::clamp(patchesPerAxis, 1,
        std::min(8, std::clamp(horizontalCellsPerChunk, 2, 48)));
    chunk->patchCount = patchAxisCount * patchAxisCount;

    for (int patchZ = 0; patchZ < patchAxisCount; ++patchZ)
    {
        for (int patchX = 0; patchX < patchAxisCount; ++patchX)
        {
            Object* patchObject = nullptr;
            for (Object* child : chunkObject->Children)
            {
                TerrainPatch* patch = child
                    ? child->GetComponent<TerrainPatch>() : nullptr;
                if (patch && patch->patchX == patchX && patch->patchZ == patchZ)
                {
                    patchObject = child;
                    break;
                }
            }
            if (!patchObject)
            {
                char name[64]{};
                std::snprintf(name, sizeof(name), "Patch (%d, %d)", patchX, patchZ);
                patchObject = AddChild(scene, *chunkObject, name);
            }

            const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
            const int startX = patchX * horizontalCells / patchAxisCount;
            const int startZ = patchZ * horizontalCells / patchAxisCount;
            patchObject->transform.position = {
                static_cast<float>(static_cast<double>(startX) *
                    std::max(1.f, chunkSize) / horizontalCells), 0.f,
                static_cast<float>(static_cast<double>(startZ) *
                    std::max(1.f, chunkSize) / horizontalCells) };

            TerrainPatch* patch = EnsureComponent<TerrainPatch>(*patchObject);
            patch->patchX = patchX;
            patch->patchZ = patchZ;
            TerrainMeshData geometry;
            std::vector<CachedPatchMesh>* sourcePatches = preparedPatches;
            if (!sourcePatches && usesCachedMesh)
                sourcePatches = &cachedChunk->second.patches;
            if (sourcePatches)
            {
                const auto cachedPatch = std::find_if(
                    sourcePatches->begin(), sourcePatches->end(),
                    [&](const CachedPatchMesh& value)
                    {
                        return value.patchX == patchX && value.patchZ == patchZ;
                    });
                if (cachedPatch != sourcePatches->end())
                    geometry = std::move(cachedPatch->geometry);
            }
            if (geometry.indices.empty())
                geometry = Engine::Components::Mesh::BuildIndexedTerrain(
                    BuildPatchVertices(chunkX, chunkZ, patchX, patchZ, *noise));
            patch->triangleCount = static_cast<int>(geometry.indices.size() / 3u);

            auto* mesh = EnsureComponent<Engine::Components::Mesh>(*patchObject);
            // Physics consumes the packed indexed terrain data directly, so a
            // collider no longer requires a second expanded vertex stream.
            mesh->SetTerrainGeometry(std::move(geometry), false);
            if (scene.GetGraphicsProvider())
            if (!mesh->GetGraphicsBuffer())
                mesh->OnAfterDeserialize(scene.GetGraphicsProvider());

            auto* material = EnsureComponent<Engine::Components::Material>(*patchObject);
            // Vertex colors carry the selected height bands. Keep the material
            // neutral so the picker values reach the shader unchanged.
            material->diffuseColor = { 1.f, 1.f, 1.f };
            material->ambientColor = { 0.08f, 0.11f, 0.07f };
            material->specularColor = { 0.18f, 0.2f, 0.16f };
            material->roughnessFactor = 0.88f;
            material->metallicFactor = 0.f;

            if (generateColliders && mesh->GetVertexCount() > 0u)
            {
                auto* collider = EnsureComponent<Engine::Components::MeshObjectCollider>(
                    *patchObject);
                collider->convex = false;
                // Activation is distance-sorted and budgeted after chunks are
                // committed so multiple Bullet BVHs cannot hitch one frame.
                if (collider->collisionEnabled)
                {
                    collider->collisionEnabled = false;
                    collider->MarkConfigurationDirty();
                }
                auto* body = EnsureComponent<Engine::Components::RigidBody>(*patchObject);
                body->bodyType = "Static";
                body->useGravity = false;
            }
        }
    }
    if (usesCachedMesh)
        m_meshCache.erase(cachedChunk);
    ++m_totalChunksBuilt;
}

void TerrainGen::ClearTerrain()
{
    // In-progress workers may finish independently, but the epoch prevents
    // their stale results from being published into replacement terrain.
    m_chunkQueue.clear();
    CancelPendingGeneration();

    if (Owner && Owner->GetScene())
    {
        Scene& scene = *Owner->GetScene();
        const std::vector<Object*> existingChildren = Owner->Children;
        for (Object* child : existingChildren)
        {
            if (child && child->GetComponent<TerrainChunk>())
                scene.RemoveObject(child);
        }
        for (Object* pooledChunk : m_chunkObjectPool)
        {
            if (pooledChunk)
                scene.RemoveObject(pooledChunk);
        }
    }

    m_chunks.clear();
    m_chunkObjectPool.clear();
    m_meshCache.clear();
    m_meshConfigurationHash = 0u;
    m_meshCacheClock = 0u;
    m_viewerChunk = {};
    m_hasViewerChunk = false;
    m_totalChunksBuilt = 0;
    m_totalChunksUnloaded = 0;
    m_meshCacheHits = 0;
    m_meshCacheMisses = 0;
    m_lastStreamingMilliseconds = 0.0;
    m_maximumStreamingMilliseconds = 0.0;
    m_pendingChunkUnloadCount = 0u;
}

bool TerrainGen::GenerateTerrain()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
    {
        m_editorGenerationStatus = "Generation failed: terrain requires a scene and noise field.";
        return false;
    }

    // Manual generation is always authoritative. Never restore geometry from
    // the streaming cache or retain chunks made with earlier settings.
    ClearTerrain();
    m_viewerChunk = ViewerChunk();
    m_hasViewerChunk = true;

    const auto generationStart = std::chrono::steady_clock::now();
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    const int64_t minimumX = std::max<int64_t>(
        std::numeric_limits<int>::lowest(),
        static_cast<int64_t>(m_viewerChunk.x) - radius);
    const int64_t maximumX = std::min<int64_t>(
        std::numeric_limits<int>::max(),
        static_cast<int64_t>(m_viewerChunk.x) + radius);
    const int64_t minimumZ = std::max<int64_t>(
        std::numeric_limits<int>::lowest(),
        static_cast<int64_t>(m_viewerChunk.y) - radius);
    const int64_t maximumZ = std::min<int64_t>(
        std::numeric_limits<int>::max(),
        static_cast<int64_t>(m_viewerChunk.y) + radius);
    for (int64_t z = minimumZ; z <= maximumZ; ++z)
    {
        for (int64_t x = minimumX; x <= maximumX; ++x)
            BuildChunk(static_cast<int>(x), static_cast<int>(z));
    }
    RefreshColliderActivation();
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - generationStart).count();
    char status[160]{};
    std::snprintf(status, sizeof(status),
        "Generated %zu chunks in %.2f ms without starting the game.",
        m_chunks.size(), milliseconds);
    m_editorGenerationStatus = status;
    return true;
}

