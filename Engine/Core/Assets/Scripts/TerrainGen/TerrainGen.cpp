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

TerrainGen::TerrainGen()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainGen));
    RegisterField("viewerObjectName", viewerObjectName);
    RegisterField("noiseObjectName", noiseObjectName);
    RegisterField("chunkSize", chunkSize);
    RegisterField("worldOriginChunkX", worldOriginChunkX);
    RegisterField("worldOriginChunkZ", worldOriginChunkZ);
    RegisterField("viewRadiusInChunks", viewRadiusInChunks);
    RegisterField("maxChunkBuildsPerUpdate", maxChunkBuildsPerUpdate);
    RegisterField("parallelChunkBuilds", parallelChunkBuilds);
    RegisterField("maxChunkCommitsPerUpdate", maxChunkCommitsPerUpdate);
    RegisterField("maxChunkUnloadsPerUpdate", maxChunkUnloadsPerUpdate);
    RegisterField("cacheUnloadedChunkMeshes", cacheUnloadedChunkMeshes);
    RegisterField("unloadedMeshCacheCapacity", unloadedMeshCacheCapacity);
    RegisterField("horizontalCellsPerChunk", horizontalCellsPerChunk);
    RegisterField("verticalCells", verticalCells);
    RegisterField("patchesPerAxis", patchesPerAxis);
    RegisterField("terrainShape", terrainShape);
    RegisterField("heightStep", heightStep);
    RegisterField("verticalSize", verticalSize);
    RegisterField("baseHeight", baseHeight);
    RegisterField("heightAmplitude", heightAmplitude);
    RegisterField("caveStrength", caveStrength);
    RegisterField("caveFrequencyMultiplier", caveFrequencyMultiplier);
    RegisterField("isoLevel", isoLevel);
    RegisterField("textureScale", textureScale);
    RegisterField("lowHeightMaximum", lowHeightMaximum);
    RegisterField("middleHeightMaximum", middleHeightMaximum);
    RegisterField("lowHeightColor", lowHeightColor);
    RegisterField("middleHeightColor", middleHeightColor);
    RegisterField("highHeightColor", highHeightColor);
    RegisterField("heightColorBlend", heightColorBlend);
    RegisterField("generateColliders", generateColliders);
    RegisterField("collisionRadiusInChunks", collisionRadiusInChunks);
    RegisterField("maxColliderActivationsPerUpdate",
        maxColliderActivationsPerUpdate);
}

TerrainGen::~TerrainGen()
{
    StopWorkerPool();
}

namespace
{
struct TerrainGenRegistration
{
    TerrainGenRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainGen>(
            "TerrainGen");
    }
};
TerrainGenRegistration g_registration;
}

int64_t TerrainGen::ChunkKey(int x, int z)
{
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) |
        static_cast<uint32_t>(z);
    return static_cast<int64_t>(key);
}

PerlinNoiseField* TerrainGen::ResolveNoise() const
{
    if (!Owner)
        return nullptr;
    if (!noiseObjectName.empty())
    {
        Object* noiseObject = Owner->FindObjectInSceneByName(noiseObjectName);
        if (noiseObject)
            return noiseObject->GetComponent<PerlinNoiseField>();
    }
    return Owner->GetComponent<PerlinNoiseField>();
}

glm::ivec2 TerrainGen::ViewerChunk() const
{
    if (!Owner)
        return {};
    Object* viewer = viewerObjectName.empty()
        ? nullptr : Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer)
        viewer = Owner;
    const glm::vec3 localPosition(glm::inverse(
        Owner->transform.GetWorldMatrix()) * glm::vec4(
            viewer->transform.GetWorldPosition(), 1.f));
    const float size = std::max(1.f, chunkSize);
    const auto logicalChunk = [size](int origin, float coordinate)
    {
        const double value = static_cast<double>(origin) +
            std::floor(static_cast<double>(coordinate) / size);
        return static_cast<int>(std::clamp(value,
            static_cast<double>(std::numeric_limits<int>::lowest()),
            static_cast<double>(std::numeric_limits<int>::max())));
    };
    return {
        logicalChunk(worldOriginChunkX, localPosition.x),
        logicalChunk(worldOriginChunkZ, localPosition.z)
    };
}

void TerrainGen::Start()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
        return;

    m_chunks.clear();
    for (Object* child : Owner->Children)
    {
        auto* chunk = child ? child->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk)
            continue;
        // Procedural vertex arrays are runtime data and are intentionally not
        // serialized into a scene. The editor may nevertheless save the
        // generated chunk/patch hierarchy. Do not register those empty shells
        // as loaded in a standalone game or streaming will skip regeneration.
        const bool hasRenderablePatch = std::any_of(child->Children.begin(),
            child->Children.end(), [](Object* patchObject)
            {
                const auto* mesh = patchObject ? patchObject->GetComponent<
                    Engine::Components::Mesh>() : nullptr;
                return mesh && mesh->GetVertexCount() > 0u;
            });
        if (!hasRenderablePatch)
        {
            Owner->GetScene()->RequestRemoveObject(child);
            continue;
        }
        m_chunks[ChunkKey(chunk->chunkX, chunk->chunkZ)] = child;
    }

    m_viewerChunk = ViewerChunk();
    m_hasViewerChunk = true;
    // Make the viewer's own chunk available immediately. Neighboring chunks
    // honor the per-update budget to avoid a large streaming hitch. Runtime
    // object insertion is deferred to Update because Scene::Start is walking
    // its stable object array.
    if (m_chunks.find(ChunkKey(m_viewerChunk.x, m_viewerChunk.y)) !=
        m_chunks.end())
    {
        BuildChunk(m_viewerChunk.x, m_viewerChunk.y);
    }
}

void TerrainGen::Update()
{
    if (!Owner || !Owner->GetScene() || !ResolveNoise())
        return;
    const glm::ivec2 current = ViewerChunk();
    if (!m_hasViewerChunk || current != m_viewerChunk)
    {
        m_viewerChunk = current;
        m_hasViewerChunk = true;
    }
    // Refresh every frame until the desired ring has filled. Once filled this
    // is a small map lookup and performs no allocations or mesh work.
    RefreshChunks();
}

void TerrainGen::OnDestroy()
{
    StopWorkerPool();
    m_chunks.clear();
    m_chunkObjectPool.clear();
    m_meshCache.clear();
    m_chunkQueue.clear();
    m_inFlightChunks.clear();
    m_readyChunks.clear();
}

void TerrainGen::FenceBackgroundWork()
{
    StopWorkerPool();
    CancelPendingGeneration();
}

void TerrainGen::StopWorkerPool()
{
    {
        std::lock_guard<std::mutex> lock(m_generationMutex);
        m_stopWorkers = true;
    }
    for (auto& job : m_generationHandles)
        if (job.valid()) job.wait();
    m_generationHandles.clear();
    {
        std::lock_guard<std::mutex> lock(m_generationMutex);
        m_completedChunks.clear();
        m_stopWorkers = false;
    }
}

void TerrainGen::CancelPendingGeneration()
{
    std::lock_guard<std::mutex> lock(m_generationMutex);
    ++m_generationEpoch;
    if (m_generationEpoch == 0)
        ++m_generationEpoch;
    m_completedChunks.clear();
    m_inFlightChunks.clear();
    m_readyChunks.clear();
}

void TerrainGen::HarvestCompletedChunks()
{
    std::deque<GeneratedChunkMesh> completed;
    {
        std::lock_guard<std::mutex> lock(m_generationMutex);
        completed.swap(m_completedChunks);
    }
    while (!completed.empty())
    {
        GeneratedChunkMesh result = std::move(completed.front());
        completed.pop_front();
        const int64_t key = ChunkKey(result.x, result.z);
        if (result.generationEpoch != m_generationEpoch ||
            m_inFlightChunks.find(key) == m_inFlightChunks.end())
            continue;
        m_readyChunks.insert_or_assign(key, std::move(result));
    }
}

void TerrainGen::RefreshChunks()
{
    if (!Owner || !Owner->GetScene() || !m_hasViewerChunk)
        return;
    const auto refreshStart = std::chrono::steady_clock::now();
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    if (const PerlinNoiseField* noise = ResolveNoise())
    {
        const uint64_t configuration = MeshConfigurationHash(*noise);
        if (m_meshConfigurationHash != 0u &&
            m_meshConfigurationHash != configuration)
            m_meshCache.clear();
        m_meshConfigurationHash = configuration;
    }

    struct UnloadCandidate
    {
        int64_t key = 0;
        Object* object = nullptr;
        int64_t distanceSquared = 0;
    };
    std::vector<UnloadCandidate> unloadCandidates;
    for (auto iterator = m_chunks.begin(); iterator != m_chunks.end();)
    {
        Object* object = iterator->second;
        TerrainChunk* chunk = object ? object->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk)
        {
            iterator = m_chunks.erase(iterator);
            continue;
        }
        // Rebasing is editable, so update existing chunks as well as new ones.
        object->transform.position = {
            static_cast<float>((static_cast<int64_t>(chunk->chunkX) -
                worldOriginChunkX) * static_cast<double>(
                    std::max(1.f, chunkSize))),
            0.f,
            static_cast<float>((static_cast<int64_t>(chunk->chunkZ) -
                worldOriginChunkZ) * static_cast<double>(
                    std::max(1.f, chunkSize))) };
        const int64_t dx = static_cast<int64_t>(chunk->chunkX) -
            m_viewerChunk.x;
        const int64_t dz = static_cast<int64_t>(chunk->chunkZ) -
            m_viewerChunk.y;
        if (std::abs(dx) > radius || std::abs(dz) > radius)
            unloadCandidates.push_back({ iterator->first, object, dx * dx + dz * dz });
        ++iterator;
    }
    std::sort(unloadCandidates.begin(), unloadCandidates.end(),
        [](const UnloadCandidate& first, const UnloadCandidate& second)
        {
            return first.distanceSquared > second.distanceSquared;
        });
    const size_t unloadBudget = static_cast<size_t>(
        std::clamp(maxChunkUnloadsPerUpdate, 1, 32));
    const size_t unloadCount = std::min(unloadBudget, unloadCandidates.size());
    for (size_t index = 0; index < unloadCount; ++index)
    {
        const UnloadCandidate& candidate = unloadCandidates[index];
        if (candidate.object)
        {
            CacheChunkMesh(candidate.key, *candidate.object);
            Owner->GetScene()->SetParentLocal(candidate.object, nullptr);
            candidate.object->enabled = false;
            m_chunkObjectPool.push_back(candidate.object);
        }
        m_chunks.erase(candidate.key);
        ++m_totalChunksUnloaded;
    }
    m_pendingChunkUnloadCount = unloadCandidates.size() - unloadCount;

    int commitBudget = std::clamp(maxChunkCommitsPerUpdate, 1, 16);
    commitBudget -= CommitCompletedChunks(commitBudget);

    m_chunkQueue.clear();
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
    for (int64_t logicalZ = minimumZ; logicalZ <= maximumZ; ++logicalZ)
    {
        for (int64_t logicalX = minimumX; logicalX <= maximumX; ++logicalX)
        {
            const int x = static_cast<int>(logicalX);
            const int z = static_cast<int>(logicalZ);
            const int64_t key = ChunkKey(x, z);
            if (m_chunks.find(key) == m_chunks.end() &&
                m_inFlightChunks.find(key) == m_inFlightChunks.end())
            {
                const int64_t dx = logicalX - m_viewerChunk.x;
                const int64_t dz = logicalZ - m_viewerChunk.y;
                m_chunkQueue.push_back({ x, z, dx * dx + dz * dz });
            }
        }
    }
    std::sort(m_chunkQueue.begin(), m_chunkQueue.end(),
        [](const QueuedChunk& first, const QueuedChunk& second)
        {
            if (first.distanceSquared != second.distanceSquared)
                return first.distanceSquared < second.distanceSquared;
            if (first.z != second.z)
                return first.z < second.z;
            return first.x < second.x;
        });

    // Cached meshes need no worker. Restore the closest cached chunks first,
    // but retain a commit budget because scene objects and GPU buffers are
    // deliberately created only on the game thread.
    for (auto iterator = m_chunkQueue.begin();
        iterator != m_chunkQueue.end() && commitBudget > 0;)
    {
        if (m_meshCache.find(ChunkKey(iterator->x, iterator->z)) ==
            m_meshCache.end())
        {
            ++iterator;
            continue;
        }
        BuildChunk(iterator->x, iterator->z);
        iterator = m_chunkQueue.erase(iterator);
        --commitBudget;
    }

    const PerlinNoiseField* noise = ResolveNoise();
    if (noise)
    {
        m_generationHandles.erase(std::remove_if(m_generationHandles.begin(),
            m_generationHandles.end(), [](std::future<void>& job)
            {
                return job.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready;
            }), m_generationHandles.end());
        const GenerationSnapshot snapshot = CaptureGenerationSnapshot(*noise);
        int launchBudget = std::clamp(maxChunkBuildsPerUpdate, 1, 16);
        const unsigned hardwareThreads = std::thread::hardware_concurrency();
        const size_t workerLimit = hardwareThreads > 1u
            ? static_cast<size_t>(hardwareThreads - 1u) : 1u;
        const size_t concurrency = std::min(workerLimit, static_cast<size_t>(
            std::clamp(parallelChunkBuilds, 1, 16)));
        while (!m_chunkQueue.empty() && launchBudget > 0 &&
            m_inFlightChunks.size() < concurrency)
        {
            const auto candidateIterator = std::find_if(m_chunkQueue.begin(),
                m_chunkQueue.end(), [this](const QueuedChunk& candidate)
                {
                    return m_meshCache.find(ChunkKey(candidate.x, candidate.z)) ==
                        m_meshCache.end();
                });
            if (candidateIterator == m_chunkQueue.end())
                break;
            const QueuedChunk candidate = *candidateIterator;
            m_chunkQueue.erase(candidateIterator);
            --launchBudget;
            const int64_t key = ChunkKey(candidate.x, candidate.z);
            m_inFlightChunks.emplace(key, InFlightChunk{
                candidate.x, candidate.z });
            const GenerationJob job{snapshot, candidate.x,
                candidate.z, m_generationEpoch};
            m_generationHandles.push_back(SubmitJob([this, job]
            {
                {
                    std::lock_guard<std::mutex> lock(m_generationMutex);
                    if (m_stopWorkers || job.generationEpoch != m_generationEpoch)
                        return;
                }
                GeneratedChunkMesh result = GenerateChunkMesh(
                    job.snapshot, job.x, job.z);
                result.generationEpoch = job.generationEpoch;
                std::lock_guard<std::mutex> lock(m_generationMutex);
                if (!m_stopWorkers && job.generationEpoch == m_generationEpoch)
                    m_completedChunks.push_back(std::move(result));
            }));
            ++m_meshCacheMisses;
        }
    }
    m_lastStreamingMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - refreshStart).count();
    m_maximumStreamingMilliseconds = std::max(m_maximumStreamingMilliseconds,
        m_lastStreamingMilliseconds);
    RefreshColliderActivation();
}

void TerrainGen::RefreshColliderActivation()
{
    const int radius = std::clamp(collisionRadiusInChunks, 0, 8);
    struct PendingCollider
    {
        Engine::Components::MeshObjectCollider* collider = nullptr;
        float distanceSquared = 0.f;
    };
    std::vector<PendingCollider> pending;
    Object* viewer = viewerObjectName.empty()
        ? nullptr : Owner->FindObjectInSceneByName(viewerObjectName);
    if (!viewer)
        viewer = Owner;
    const glm::vec3 viewerPosition = viewer
        ? viewer->transform.GetWorldPosition() : glm::vec3(0.f);
    for (const auto& [key, chunkObject] : m_chunks)
    {
        (void)key;
        TerrainChunk* chunk = chunkObject
            ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk)
            continue;
        const bool active = generateColliders &&
            std::abs(static_cast<int64_t>(chunk->chunkX) - m_viewerChunk.x) <= radius &&
            std::abs(static_cast<int64_t>(chunk->chunkZ) - m_viewerChunk.y) <= radius;
        for (Object* patchObject : chunkObject->Children)
        {
            if (!patchObject)
                continue;
            auto* collider = patchObject->GetComponent<
                Engine::Components::MeshObjectCollider>();
            if (!collider && generateColliders)
            {
                collider = EnsureComponent<
                    Engine::Components::MeshObjectCollider>(*patchObject);
                collider->convex = false;
                auto* body = EnsureComponent<Engine::Components::RigidBody>(
                    *patchObject);
                body->bodyType = "Static";
                body->useGravity = false;
            }
            if (!collider)
                continue;
            if (!active && collider->collisionEnabled)
            {
                collider->collisionEnabled = false;
                collider->MarkConfigurationDirty();
            }
            else if (active && !collider->collisionEnabled)
            {
                const auto* mesh = patchObject->GetComponent<
                    Engine::Components::Mesh>();
                glm::vec3 center = patchObject->transform.GetWorldPosition();
                if (mesh && mesh->HasBounds())
                {
                    center = glm::vec3(patchObject->transform.GetWorldMatrix() *
                        glm::vec4((mesh->GetBoundsMin() + mesh->GetBoundsMax()) *
                            0.5f, 1.f));
                }
                const glm::vec2 delta(center.x - viewerPosition.x,
                    center.z - viewerPosition.z);
                pending.push_back({ collider, glm::dot(delta, delta) });
            }
        }
    }
    std::sort(pending.begin(), pending.end(),
        [](const PendingCollider& first, const PendingCollider& second)
        { return first.distanceSquared < second.distanceSquared; });
    const size_t activationCount = std::min(pending.size(),
        static_cast<size_t>(std::clamp(maxColliderActivationsPerUpdate, 1, 16)));
    for (size_t index = 0; index < activationCount; ++index)
    {
        pending[index].collider->collisionEnabled = true;
        pending[index].collider->MarkConfigurationDirty();
    }
}

