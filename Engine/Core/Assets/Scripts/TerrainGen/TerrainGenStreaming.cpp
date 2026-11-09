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

bool TerrainGen::IsChunkDesired(int x, int z) const
{
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    return std::abs(x - m_viewerChunk.x) <= radius &&
        std::abs(z - m_viewerChunk.y) <= radius;
}

int TerrainGen::CommitCompletedChunks(int budget)
{
    HarvestCompletedChunks();
    struct ReadyTask { int64_t key = 0; int64_t distanceSquared = 0; };
    std::vector<ReadyTask> ready;
    for (const auto& [key, result] : m_readyChunks)
    {
        const int64_t dx = static_cast<int64_t>(result.x) - m_viewerChunk.x;
        const int64_t dz = static_cast<int64_t>(result.z) - m_viewerChunk.y;
        ready.push_back({ key, dx * dx + dz * dz });
    }
    std::sort(ready.begin(), ready.end(),
        [](const ReadyTask& first, const ReadyTask& second)
        {
            return first.distanceSquared < second.distanceSquared;
        });

    int committed = 0;
    for (const ReadyTask& readyTask : ready)
    {
        auto readyIterator = m_readyChunks.find(readyTask.key);
        if (readyIterator == m_readyChunks.end())
            continue;
        const int chunkX = readyIterator->second.x;
        const int chunkZ = readyIterator->second.z;
        const bool desired = IsChunkDesired(chunkX, chunkZ);
        const bool viewerSeed = chunkX == m_viewerChunk.x &&
            chunkZ == m_viewerChunk.y;
        const bool touchesLoadedChunk = viewerSeed ||
            m_chunks.find(ChunkKey(chunkX - 1, chunkZ)) != m_chunks.end() ||
            m_chunks.find(ChunkKey(chunkX + 1, chunkZ)) != m_chunks.end() ||
            m_chunks.find(ChunkKey(chunkX, chunkZ - 1)) != m_chunks.end() ||
            m_chunks.find(ChunkKey(chunkX, chunkZ + 1)) != m_chunks.end();
        // Keep the visible terrain edge connected. A worker may finish a more
        // distant chunk first, but exposing it before an adjacent chunk creates
        // temporary islands and obvious gaps during streaming.
        if (desired && (committed >= budget || !touchesLoadedChunk))
            continue;
        GeneratedChunkMesh result = std::move(readyIterator->second);
        m_readyChunks.erase(readyIterator);
        m_inFlightChunks.erase(readyTask.key);
        if (!desired || result.configurationHash != m_meshConfigurationHash ||
            m_chunks.find(readyTask.key) != m_chunks.end())
            continue;
        BuildChunk(result.x, result.z, &result.patches);
        ++committed;
    }
    return committed;
}

uint64_t TerrainGen::MeshConfigurationHash(
    const PerlinNoiseField& noise) const
{
    uint64_t hash = 1469598103934665603ull;
    const auto addBytes = [&](const void* data, size_t byteCount)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t index = 0; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    const auto add = [&](const auto& value) { addBytes(&value, sizeof(value)); };
    add(chunkSize); add(horizontalCellsPerChunk); add(verticalCells);
    add(patchesPerAxis); add(terrainShape); add(heightStep);
    add(verticalSize); add(baseHeight); add(heightAmplitude); add(caveStrength);
    add(caveFrequencyMultiplier); add(isoLevel); add(textureScale);
    add(lowHeightMaximum); add(middleHeightMaximum);
    add(lowHeightColor); add(middleHeightColor); add(highHeightColor);
    add(heightColorBlend);
    add(noise.seed); add(noise.frequency); add(noise.octaves);
    add(noise.lacunarity); add(noise.persistence); add(noise.coordinateOffset);
    return hash;
}

void TerrainGen::CacheChunkMesh(int64_t key,
    Engine::Core::Object& chunkObject)
{
    if (!cacheUnloadedChunkMeshes || unloadedMeshCacheCapacity <= 0)
        return;
    CachedChunkMesh cached;
    cached.lastUse = ++m_meshCacheClock;
    cached.patches.reserve(chunkObject.Children.size());
    for (Object* child : chunkObject.Children)
    {
        const TerrainPatch* patch = child
            ? child->GetComponent<TerrainPatch>() : nullptr;
        auto* mesh = child
            ? child->GetComponent<Engine::Components::Mesh>() : nullptr;
        if (!patch || !mesh || !mesh->UsesTerrainVertexFormat())
            continue;
        cached.patches.push_back({ patch->patchX, patch->patchZ,
            mesh->TakeTerrainGeometry() });
    }
    if (!cached.patches.empty())
    {
        m_meshCache[key] = std::move(cached);
        TrimMeshCache();
    }
}

void TerrainGen::TrimMeshCache()
{
    const size_t capacity = static_cast<size_t>(
        std::clamp(unloadedMeshCacheCapacity, 0, 1024));
    while (m_meshCache.size() > capacity)
    {
        auto oldest = m_meshCache.end();
        for (auto iterator = m_meshCache.begin(); iterator != m_meshCache.end();
            ++iterator)
        {
            if (oldest == m_meshCache.end() ||
                iterator->second.lastUse < oldest->second.lastUse)
                oldest = iterator;
        }
        if (oldest == m_meshCache.end())
            break;
        m_meshCache.erase(oldest);
    }
}

