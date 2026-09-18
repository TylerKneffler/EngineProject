#include "Scripts/TerrainGen/TerrainGen.h"

#include "Scripts/TerrainGen/TerrainChunk.h"
#include "Scripts/TerrainGen/TerrainPatch.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/Collider.h"
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
#include <cstring>
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
    child->Parent = &parent;
    parent.Children.push_back(child);
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
    RegisterField("generateColliders", generateColliders);
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
    return {
        static_cast<int>(std::floor(localPosition.x / size)),
        static_cast<int>(std::floor(localPosition.z / size))
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

void TerrainGen::EnsureWorkerPool(std::size_t workerCount)
{
    workerCount = std::max<std::size_t>(1u, workerCount);
    while (m_workerThreads.size() < workerCount)
        m_workerThreads.emplace_back([this]() { WorkerLoop(); });
}

void TerrainGen::StopWorkerPool()
{
    {
        std::lock_guard<std::mutex> lock(m_generationMutex);
        m_stopWorkers = true;
        m_generationJobs.clear();
    }
    m_generationCondition.notify_all();
    for (std::thread& worker : m_workerThreads)
        if (worker.joinable())
            worker.join();
    m_workerThreads.clear();
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
    m_generationJobs.clear();
    m_completedChunks.clear();
    m_inFlightChunks.clear();
    m_readyChunks.clear();
}

void TerrainGen::WorkerLoop()
{
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;)
    {
        GenerationJob job;
        {
            std::unique_lock<std::mutex> lock(m_generationMutex);
            m_generationCondition.wait(lock, [this]()
            {
                return m_stopWorkers || !m_generationJobs.empty();
            });
            if (m_stopWorkers)
                return;
            job = std::move(m_generationJobs.front());
            m_generationJobs.pop_front();
        }
        GeneratedChunkMesh result = GenerateChunkMesh(
            job.snapshot, job.x, job.z);
        result.generationEpoch = job.generationEpoch;
        {
            std::lock_guard<std::mutex> lock(m_generationMutex);
            if (!m_stopWorkers)
                m_completedChunks.push_back(std::move(result));
        }
    }
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
        int distanceSquared = 0;
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
        const int dx = chunk->chunkX - m_viewerChunk.x;
        const int dz = chunk->chunkZ - m_viewerChunk.y;
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
            if (candidate.object->Parent)
            {
                auto& siblings = candidate.object->Parent->Children;
                siblings.erase(std::remove(siblings.begin(), siblings.end(),
                    candidate.object), siblings.end());
            }
            candidate.object->Parent = nullptr;
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
    for (int z = m_viewerChunk.y - radius; z <= m_viewerChunk.y + radius; ++z)
    {
        for (int x = m_viewerChunk.x - radius; x <= m_viewerChunk.x + radius; ++x)
        {
            const int64_t key = ChunkKey(x, z);
            if (m_chunks.find(key) == m_chunks.end() &&
                m_inFlightChunks.find(key) == m_inFlightChunks.end())
            {
                const int dx = x - m_viewerChunk.x;
                const int dz = z - m_viewerChunk.y;
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
            EnsureWorkerPool(concurrency);
            m_inFlightChunks.emplace(key, InFlightChunk{
                candidate.x, candidate.z });
            {
                std::lock_guard<std::mutex> lock(m_generationMutex);
                m_generationJobs.push_back({ snapshot, candidate.x,
                    candidate.z, m_generationEpoch });
            }
            m_generationCondition.notify_one();
            ++m_meshCacheMisses;
        }
    }
    m_lastStreamingMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - refreshStart).count();
    m_maximumStreamingMilliseconds = std::max(m_maximumStreamingMilliseconds,
        m_lastStreamingMilliseconds);
}

float TerrainGen::Density(const glm::vec3& position,
    const PerlinNoiseField& noise) const
{
    const float surface = baseHeight + heightAmplitude *
        noise.SampleFractal2D(position.x, position.z);
    const float caves = caveStrength > 0.f
        ? caveStrength * noise.SampleFractal(
            position * std::max(0.01f, caveFrequencyMultiplier))
        : 0.f;
    // Negative is solid and positive is air.
    return position.y - surface + caves;
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildPatchVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    switch (static_cast<TerrainShape>(terrainShape))
    {
    case TerrainShape::Cubes:
        return BuildCubeVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::Triangles:
        return BuildTriangleVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::Hexagons:
        return BuildHexagonVertices(chunkX, chunkZ, patchX, patchZ, noise);
    case TerrainShape::SmoothSurface:
    default:
        return BuildSmoothSurfaceVertices(chunkX, chunkZ, patchX, patchZ, noise);
    }
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildSmoothSurfaceVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int yCells = std::clamp(verticalCells, 2, 48);
    const int patchAxisCount = std::clamp(patchesPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = patchX * horizontalCells / patchAxisCount;
    const int endX = (patchX + 1) * horizontalCells / patchAxisCount;
    const int startZ = patchZ * horizontalCells / patchAxisCount;
    const int endZ = (patchZ + 1) * horizontalCells / patchAxisCount;
    const float terrainChunkSize = std::max(1.f, chunkSize);
    const float horizontalStep = terrainChunkSize /
        static_cast<float>(horizontalCells);
    const float ySize = std::max(1.f, verticalSize);
    const float verticalStep = ySize / static_cast<float>(yCells);
    const float minimumY = -ySize * 0.5f;
    const glm::vec3 patchOrigin(
        static_cast<float>(static_cast<double>(startX) * terrainChunkSize /
            horizontalCells), 0.f,
        static_cast<float>(static_cast<double>(startZ) * terrainChunkSize /
            horizontalCells));
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * terrainChunkSize, 0.f,
        static_cast<float>(chunkZ) * terrainChunkSize);
    // Use one global integer lattice for samples shared by neighboring chunks.
    // This avoids small density/position disagreements caused by reaching the
    // same border through different floating-point addition paths.
    const auto worldGridCoordinate = [horizontalCells, terrainChunkSize](
        int chunk, int cell)
    {
        const int64_t globalCell = static_cast<int64_t>(chunk) *
            horizontalCells + cell;
        return static_cast<float>(static_cast<double>(globalCell) *
            static_cast<double>(terrainChunkSize) /
            static_cast<double>(horizontalCells));
    };

    static constexpr int cubeCorners[8][3] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 },
        { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 }
    };
    static constexpr int cubeEdges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
    };
    static constexpr int cubeFaces[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }
    };
    static constexpr int faceEdges[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 9, 4, 8 },
        { 1, 10, 5, 9 }, { 2, 11, 6, 10 }, { 3, 8, 7, 11 }
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * yCells * 18u);

    // This cache belongs only to this new patch build. It avoids recomputing
    // shared cell corners and derives normals from the same density field; it
    // is unrelated to the persistent cache used when revisiting old chunks.
    const int gridWidth = endX - startX + 3;
    const int gridDepth = endZ - startZ + 3;
    const int gridHeight = yCells + 3;
    const auto gridIndex = [=](int x, int y, int z)
    {
        return (static_cast<size_t>(y) * gridDepth + z) * gridWidth + x;
    };
    std::vector<float> surfaceHeights(static_cast<size_t>(gridWidth) * gridDepth);
    for (int z = 0; z < gridDepth; ++z)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            const float worldX = worldGridCoordinate(chunkX, startX + x - 1);
            const float worldZ = worldGridCoordinate(chunkZ, startZ + z - 1);
            surfaceHeights[static_cast<size_t>(z) * gridWidth + x] =
                baseHeight + heightAmplitude * noise.SampleFractal2D(worldX, worldZ);
        }
    }
    std::vector<float> densityGrid(static_cast<size_t>(gridWidth) *
        gridDepth * gridHeight);
    for (int y = 0; y < gridHeight; ++y)
    {
        const float worldY = minimumY + static_cast<float>(y - 1) * verticalStep;
        for (int z = 0; z < gridDepth; ++z)
        {
            const float worldZ = worldGridCoordinate(chunkZ, startZ + z - 1);
            for (int x = 0; x < gridWidth; ++x)
            {
                const float worldX = worldGridCoordinate(chunkX, startX + x - 1);
                const float caves = caveStrength > 0.f
                    ? caveStrength * noise.SampleFractal(glm::vec3(
                        worldX, worldY, worldZ) *
                        std::max(0.01f, caveFrequencyMultiplier))
                    : 0.f;
                densityGrid[gridIndex(x, y, z)] = worldY -
                    surfaceHeights[static_cast<size_t>(z) * gridWidth + x] + caves;
            }
        }
    }
    const auto gradientAt = [&](int x, int y, int z)
    {
        glm::vec3 result(
            (densityGrid[gridIndex(x + 1, y, z)] -
                densityGrid[gridIndex(x - 1, y, z)]) / (2.f * horizontalStep),
            (densityGrid[gridIndex(x, y + 1, z)] -
                densityGrid[gridIndex(x, y - 1, z)]) / (2.f * verticalStep),
            (densityGrid[gridIndex(x, y, z + 1)] -
                densityGrid[gridIndex(x, y, z - 1)]) / (2.f * horizontalStep));
        const float length = glm::length(result);
        return length > 0.000001f ? result / length : glm::vec3(0.f, 1.f, 0.f);
    };

    const auto makeVertex = [&](const glm::vec3& terrainPosition,
        const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::vec3 local = terrainPosition - chunkOrigin - patchOrigin;
        glm::vec3 tangent = glm::vec3(1.f, 0.f, 0.f) - normal * normal.x;
        if (glm::length(tangent) < 0.0001f)
            tangent = glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
        vertex.uv[0] = terrainPosition.x / std::max(0.01f, textureScale);
        vertex.uv[1] = terrainPosition.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(terrainPosition.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };

    const auto emitPolygon = [&](const std::array<glm::vec3, 12>& polygon,
        const std::array<glm::vec3, 12>& polygonNormals, int polygonSize)
    {
        if (polygonSize < 3)
            return;
        std::array<glm::vec3, 12> compactPolygon{};
        std::array<glm::vec3, 12> compactNormals{};
        int compactSize = 0;
        const float mergeDistance =
            std::max(horizontalStep, verticalStep) * 0.00001f;
        const float mergeDistanceSquared = mergeDistance * mergeDistance;
        for (int index = 0; index < polygonSize; ++index)
        {
            bool duplicate = false;
            for (int prior = 0; prior < compactSize; ++prior)
            {
                const glm::vec3 delta =
                    polygon[index] - compactPolygon[prior];
                if (glm::dot(delta, delta) <= mergeDistanceSquared)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                compactPolygon[compactSize] = polygon[index];
                compactNormals[compactSize] = polygonNormals[index];
                ++compactSize;
            }
        }
        if (compactSize < 3)
            return;
        glm::vec3 faceNormal(0.f);
        for (int index = 0; index < compactSize; ++index)
            faceNormal += compactNormals[index];
        faceNormal = glm::length(faceNormal) > 0.0001f
            ? glm::normalize(faceNormal) : glm::vec3(0.f, 1.f, 0.f);
        for (int index = 1; index + 1 < compactSize; ++index)
        {
            glm::vec3 first = compactPolygon[0];
            glm::vec3 second = compactPolygon[index];
            glm::vec3 third = compactPolygon[index + 1];
            glm::vec3 firstNormal = compactNormals[0];
            glm::vec3 secondNormal = compactNormals[index];
            glm::vec3 thirdNormal = compactNormals[index + 1];
            const glm::vec3 faceCross =
                glm::cross(second - first, third - first);
            if (glm::dot(faceCross, faceCross) <=
                mergeDistanceSquared * mergeDistanceSquared)
                continue;
            if (glm::dot(faceCross, faceNormal) < 0.f)
            {
                std::swap(second, third);
                std::swap(secondNormal, thirdNormal);
            }
            vertices.push_back(makeVertex(first, firstNormal));
            vertices.push_back(makeVertex(second, secondNormal));
            vertices.push_back(makeVertex(third, thirdNormal));
        }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            for (int y = 0; y < yCells; ++y)
            {
                std::array<glm::vec3, 8> positions{};
                std::array<glm::vec3, 8> cornerGradients{};
                std::array<float, 8> densities{};
                for (int corner = 0; corner < 8; ++corner)
                {
                    positions[corner] = glm::vec3(
                        worldGridCoordinate(chunkX, x + cubeCorners[corner][0]),
                        minimumY + static_cast<float>(y + cubeCorners[corner][1]) * verticalStep,
                        worldGridCoordinate(chunkZ, z + cubeCorners[corner][2]));
                    const int gridX = x - startX + cubeCorners[corner][0] + 1;
                    const int gridY = y + cubeCorners[corner][1] + 1;
                    const int gridZ = z - startZ + cubeCorners[corner][2] + 1;
                    densities[corner] = densityGrid[gridIndex(gridX, gridY, gridZ)];
                    cornerGradients[corner] = gradientAt(gridX, gridY, gridZ);
                }

                std::array<glm::vec3, 12> intersections{};
                std::array<glm::vec3, 12> intersectionNormals{};
                std::array<bool, 12> activeEdges{};
                for (int edge = 0; edge < 12; ++edge)
                {
                    int first = cubeEdges[edge][0];
                    int second = cubeEdges[edge][1];
                    if ((densities[first] < isoLevel) ==
                        (densities[second] < isoLevel))
                        continue;
                    // A shared lattice edge must take the same arithmetic path
                    // from either adjacent cell/chunk. Canonically ordering its
                    // world-space endpoints prevents sub-pixel raster cracks.
                    const auto pointLess = [&](int left, int right)
                    {
                        if (positions[left].x != positions[right].x)
                            return positions[left].x < positions[right].x;
                        if (positions[left].y != positions[right].y)
                            return positions[left].y < positions[right].y;
                        return positions[left].z < positions[right].z;
                    };
                    if (pointLess(second, first))
                        std::swap(first, second);
                    const float denominator = densities[second] - densities[first];
                    float amount = std::abs(denominator) > 0.000001f
                        ? std::clamp((isoLevel - densities[first]) / denominator,
                            0.f, 1.f)
                        : 0.5f;
                    // Resolve near-corner crossings to the exact shared
                    // lattice point. Otherwise incident edges can create
                    // duplicate sliver triangles with slightly different
                    // endpoint positions.
                    constexpr float endpointSnap = 0.00001f;
                    if (amount <= endpointSnap)
                        amount = 0.f;
                    else if (amount >= 1.f - endpointSnap)
                        amount = 1.f;
                    activeEdges[edge] = true;
                    intersections[edge] = positions[first] +
                        (positions[second] - positions[first]) * amount;
                    const glm::vec3 interpolatedGradient = cornerGradients[first] +
                        (cornerGradients[second] - cornerGradients[first]) * amount;
                    intersectionNormals[edge] = glm::length(interpolatedGradient) >
                        0.000001f ? glm::normalize(interpolatedGradient) :
                        glm::vec3(0.f, 1.f, 0.f);
                }

                std::array<std::array<int, 2>, 12> neighbors{};
                std::array<int, 12> neighborCounts{};
                for (auto& edgeNeighbors : neighbors)
                    edgeNeighbors = { -1, -1 };
                const auto connect = [&](int first, int second)
                {
                    if (first == second)
                        return;
                    if (neighborCounts[first] < 2)
                        neighbors[first][neighborCounts[first]++] = second;
                    if (neighborCounts[second] < 2)
                        neighbors[second][neighborCounts[second]++] = first;
                };

                for (int face = 0; face < 6; ++face)
                {
                    std::array<int, 4> crossed{};
                    int crossingCount = 0;
                    for (int index = 0; index < 4; ++index)
                    {
                        const int edge = faceEdges[face][index];
                        if (activeEdges[edge])
                            crossed[crossingCount++] = edge;
                    }
                    if (crossingCount == 2)
                    {
                        connect(crossed[0], crossed[1]);
                    }
                    else if (crossingCount == 4)
                    {
                        double centerDensity = 0.0;
                        std::array<int, 4> orderedCorners {
                            cubeFaces[face][0], cubeFaces[face][1],
                            cubeFaces[face][2], cubeFaces[face][3] };
                        std::sort(orderedCorners.begin(), orderedCorners.end(),
                            [&](int left, int right)
                            {
                                if (positions[left].x != positions[right].x)
                                    return positions[left].x < positions[right].x;
                                if (positions[left].y != positions[right].y)
                                    return positions[left].y < positions[right].y;
                                return positions[left].z < positions[right].z;
                            });
                        for (int corner : orderedCorners)
                            centerDensity += static_cast<double>(densities[corner]);
                        centerDensity *= 0.25;
                        const bool centerMatchesFirst =
                            (centerDensity < static_cast<double>(isoLevel)) ==
                            (densities[cubeFaces[face][0]] < isoLevel);
                        if (centerMatchesFirst)
                        {
                            connect(faceEdges[face][0], faceEdges[face][1]);
                            connect(faceEdges[face][2], faceEdges[face][3]);
                        }
                        else
                        {
                            connect(faceEdges[face][3], faceEdges[face][0]);
                            connect(faceEdges[face][1], faceEdges[face][2]);
                        }
                    }
                }

                std::array<bool, 12> visited{};
                for (int start = 0; start < 12; ++start)
                {
                    if (!activeEdges[start] || visited[start] ||
                        neighborCounts[start] == 0)
                        continue;
                    std::array<glm::vec3, 12> polygon{};
                    std::array<glm::vec3, 12> polygonNormals{};
                    int polygonSize = 0;
                    int previous = -1;
                    int current = start;
                    for (int step = 0; step < 12; ++step)
                    {
                        polygon[polygonSize] = intersections[current];
                        polygonNormals[polygonSize] = intersectionNormals[current];
                        ++polygonSize;
                        visited[current] = true;
                        const int next = neighbors[current][0] != previous
                            ? neighbors[current][0] : neighbors[current][1];
                        if (next < 0 || next == start || visited[next])
                            break;
                        previous = current;
                        current = next;
                    }
                    emitPolygon(polygon, polygonNormals, polygonSize);
                }
            }
        }
    }
    return vertices;
}

TerrainGen::GenerationSnapshot
TerrainGen::CaptureGenerationSnapshot(
    const PerlinNoiseField& noise) const
{
    GenerationSnapshot snapshot;
    snapshot.chunkSize = chunkSize;
    snapshot.horizontalCells = horizontalCellsPerChunk;
    snapshot.verticalCells = verticalCells;
    snapshot.patchesPerAxis = patchesPerAxis;
    snapshot.terrainShape = terrainShape;
    snapshot.heightStep = heightStep;
    snapshot.verticalSize = verticalSize;
    snapshot.baseHeight = baseHeight;
    snapshot.heightAmplitude = heightAmplitude;
    snapshot.caveStrength = caveStrength;
    snapshot.caveFrequencyMultiplier = caveFrequencyMultiplier;
    snapshot.isoLevel = isoLevel;
    snapshot.textureScale = textureScale;
    snapshot.lowHeightMaximum = lowHeightMaximum;
    snapshot.middleHeightMaximum = middleHeightMaximum;
    snapshot.lowHeightColor = lowHeightColor;
    snapshot.middleHeightColor = middleHeightColor;
    snapshot.highHeightColor = highHeightColor;
    snapshot.noiseSeed = noise.seed;
    snapshot.noiseFrequency = noise.frequency;
    snapshot.noiseOctaves = noise.octaves;
    snapshot.noiseLacunarity = noise.lacunarity;
    snapshot.noisePersistence = noise.persistence;
    snapshot.noiseOffset = noise.coordinateOffset;
    snapshot.configurationHash = MeshConfigurationHash(noise);
    return snapshot;
}

TerrainGen::GeneratedChunkMesh
TerrainGen::GenerateChunkMesh(const GenerationSnapshot& snapshot,
    int chunkX, int chunkZ)
{
    TerrainGen generator;
    generator.chunkSize = snapshot.chunkSize;
    generator.horizontalCellsPerChunk = snapshot.horizontalCells;
    generator.verticalCells = snapshot.verticalCells;
    generator.patchesPerAxis = snapshot.patchesPerAxis;
    generator.terrainShape = snapshot.terrainShape;
    generator.heightStep = snapshot.heightStep;
    generator.verticalSize = snapshot.verticalSize;
    generator.baseHeight = snapshot.baseHeight;
    generator.heightAmplitude = snapshot.heightAmplitude;
    generator.caveStrength = snapshot.caveStrength;
    generator.caveFrequencyMultiplier = snapshot.caveFrequencyMultiplier;
    generator.isoLevel = snapshot.isoLevel;
    generator.textureScale = snapshot.textureScale;
    generator.lowHeightMaximum = snapshot.lowHeightMaximum;
    generator.middleHeightMaximum = snapshot.middleHeightMaximum;
    generator.lowHeightColor = snapshot.lowHeightColor;
    generator.middleHeightColor = snapshot.middleHeightColor;
    generator.highHeightColor = snapshot.highHeightColor;

    PerlinNoiseField noise;
    noise.seed = snapshot.noiseSeed;
    noise.frequency = snapshot.noiseFrequency;
    noise.octaves = snapshot.noiseOctaves;
    noise.lacunarity = snapshot.noiseLacunarity;
    noise.persistence = snapshot.noisePersistence;
    noise.coordinateOffset = snapshot.noiseOffset;

    GeneratedChunkMesh result;
    result.x = chunkX;
    result.z = chunkZ;
    result.configurationHash = snapshot.configurationHash;
    const int patchCount = std::clamp(snapshot.patchesPerAxis, 1,
        std::min(8, std::clamp(snapshot.horizontalCells, 2, 48)));
    result.patches.reserve(static_cast<size_t>(patchCount * patchCount));
    for (int patchZ = 0; patchZ < patchCount; ++patchZ)
    {
        for (int patchX = 0; patchX < patchCount; ++patchX)
        {
            result.patches.push_back({ patchX, patchZ,
                Engine::Components::Mesh::BuildIndexedTerrain(
                    generator.BuildPatchVertices(chunkX, chunkZ, patchX,
                        patchZ, noise)) });
        }
    }
    return result;
}

bool TerrainGen::IsChunkDesired(int x, int z) const
{
    const int radius = std::clamp(viewRadiusInChunks, 0, 8);
    return std::abs(x - m_viewerChunk.x) <= radius &&
        std::abs(z - m_viewerChunk.y) <= radius;
}

int TerrainGen::CommitCompletedChunks(int budget)
{
    HarvestCompletedChunks();
    struct ReadyTask { int64_t key = 0; int distanceSquared = 0; };
    std::vector<ReadyTask> ready;
    for (const auto& [key, result] : m_readyChunks)
    {
        const int dx = result.x - m_viewerChunk.x;
        const int dz = result.z - m_viewerChunk.y;
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

float TerrainGen::SteppedHeight(float worldX, float worldZ,
    const PerlinNoiseField& noise) const
{
    const float step = std::max(0.05f, heightStep);
    const float rawHeight = baseHeight + heightAmplitude *
        noise.SampleFractal2D(worldX, worldZ);
    const float halfHeight = std::max(1.f, verticalSize) * 0.5f;
    return std::clamp(std::round(rawHeight / step) * step,
        -halfHeight, halfHeight);
}

glm::vec3 TerrainGen::ColorForHeight(float height) const
{
    const float lower = std::min(lowHeightMaximum, middleHeightMaximum);
    const float upper = std::max(lowHeightMaximum, middleHeightMaximum);
    if (height <= lower)
        return glm::clamp(lowHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    if (height <= upper)
        return glm::clamp(middleHeightColor, glm::vec3(0.f), glm::vec3(1.f));
    return glm::clamp(highHeightColor, glm::vec3(0.f), glm::vec3(1.f));
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildCubeVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int horizontalCells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchAxisCount = std::clamp(patchesPerAxis, 1,
        std::min(8, horizontalCells));
    const int startX = patchX * horizontalCells / patchAxisCount;
    const int endX = (patchX + 1) * horizontalCells / patchAxisCount;
    const int startZ = patchZ * horizontalCells / patchAxisCount;
    const int endZ = (patchZ + 1) * horizontalCells / patchAxisCount;
    const float cellSize = std::max(1.f, chunkSize) /
        static_cast<float>(horizontalCells);
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * std::max(1.f, chunkSize), 0.f,
        static_cast<float>(chunkZ) * std::max(1.f, chunkSize));
    const glm::vec3 patchOrigin(
        static_cast<float>(startX) * cellSize, 0.f,
        static_cast<float>(startZ) * cellSize);

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * 30u);

    const auto makeVertex = [&](const glm::vec3& terrainPosition,
        const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::vec3 local = terrainPosition - chunkOrigin - patchOrigin;
        glm::vec3 tangent = std::abs(normal.x) < 0.9f
            ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        tangent -= normal * glm::dot(tangent, normal);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = terrainPosition.x / std::max(0.01f, textureScale);
        vertex.uv[1] = terrainPosition.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(terrainPosition.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };
    const auto emitQuad = [&](glm::vec3 first, glm::vec3 second,
        glm::vec3 third, glm::vec3 fourth, const glm::vec3& normal)
    {
        if (glm::dot(glm::cross(second - first, third - first), normal) < 0.f)
            std::swap(second, fourth);
        vertices.push_back(makeVertex(first, normal));
        vertices.push_back(makeVertex(second, normal));
        vertices.push_back(makeVertex(third, normal));
        vertices.push_back(makeVertex(first, normal));
        vertices.push_back(makeVertex(third, normal));
        vertices.push_back(makeVertex(fourth, normal));
    };
    // Include a one-cell halo so every shared top/wall decision is sampled
    // exactly once for this mesh batch. The previous per-face calls evaluated
    // the same four-octave noise value as many as five times per terrain cell.
    const int sampledWidth = endX - startX + 2;
    const int sampledDepth = endZ - startZ + 2;
    std::vector<float> sampledHeights(static_cast<size_t>(sampledWidth) *
        static_cast<size_t>(sampledDepth));
    for (int sampleZ = 0; sampleZ < sampledDepth; ++sampleZ)
    {
        for (int sampleX = 0; sampleX < sampledWidth; ++sampleX)
        {
            const int cellX = startX + sampleX - 1;
            const int cellZ = startZ + sampleZ - 1;
            sampledHeights[static_cast<size_t>(sampleZ) * sampledWidth +
                sampleX] = SteppedHeight(
                    chunkOrigin.x + (static_cast<float>(cellX) + 0.5f) * cellSize,
                    chunkOrigin.z + (static_cast<float>(cellZ) + 0.5f) * cellSize,
                    noise);
        }
    }
    const auto heightAtCell = [&](int cellX, int cellZ)
    {
        const int sampleX = cellX - startX + 1;
        const int sampleZ = cellZ - startZ + 1;
        return sampledHeights[static_cast<size_t>(sampleZ) * sampledWidth +
            sampleX];
    };

    static constexpr int directions[4][2] = {
        { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }
    };
    static constexpr glm::vec3 normals[4] = {
        { -1.f, 0.f, 0.f }, { 1.f, 0.f, 0.f },
        { 0.f, 0.f, -1.f }, { 0.f, 0.f, 1.f }
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            const float x0 = chunkOrigin.x + static_cast<float>(x) * cellSize;
            const float x1 = x0 + cellSize;
            const float z0 = chunkOrigin.z + static_cast<float>(z) * cellSize;
            const float z1 = z0 + cellSize;
            const float height = heightAtCell(x, z);
            emitQuad({ x0, height, z0 }, { x0, height, z1 },
                { x1, height, z1 }, { x1, height, z0 }, { 0.f, 1.f, 0.f });

            for (int direction = 0; direction < 4; ++direction)
            {
                const float neighborHeight = heightAtCell(
                    x + directions[direction][0],
                    z + directions[direction][1]);
                if (height <= neighborHeight + 0.0001f)
                    continue;
                if (direction == 0)
                    emitQuad({ x0, neighborHeight, z0 }, { x0, neighborHeight, z1 },
                        { x0, height, z1 }, { x0, height, z0 }, normals[direction]);
                else if (direction == 1)
                    emitQuad({ x1, neighborHeight, z1 }, { x1, neighborHeight, z0 },
                        { x1, height, z0 }, { x1, height, z1 }, normals[direction]);
                else if (direction == 2)
                    emitQuad({ x1, neighborHeight, z0 }, { x0, neighborHeight, z0 },
                        { x0, height, z0 }, { x1, height, z0 }, normals[direction]);
                else
                    emitQuad({ x0, neighborHeight, z1 }, { x1, neighborHeight, z1 },
                        { x1, height, z1 }, { x0, height, z1 }, normals[direction]);
            }
        }
    }
    return vertices;
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildTriangleVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int cells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchCount = std::clamp(patchesPerAxis, 1, std::min(8, cells));
    const int startX = patchX * cells / patchCount;
    const int endX = (patchX + 1) * cells / patchCount;
    const int startZ = patchZ * cells / patchCount;
    const int endZ = (patchZ + 1) * cells / patchCount;
    const float size = std::max(1.f, chunkSize);
    const float cellSize = size / static_cast<float>(cells);
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * size, 0.f,
        static_cast<float>(chunkZ) * size);
    const glm::vec3 patchOrigin(
        static_cast<float>(startX) * cellSize, 0.f,
        static_cast<float>(startZ) * cellSize);

    const auto surfaceHeight = [&](float x, float z)
    {
        const float halfHeight = std::max(1.f, verticalSize) * 0.5f;
        return std::clamp(baseHeight + heightAmplitude *
            noise.SampleFractal2D(x, z), -halfHeight, halfHeight);
    };
    const auto makeVertex = [&](const glm::vec3& point, const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::vec3 local = point - chunkOrigin - patchOrigin;
        glm::vec3 tangent = glm::vec3(1.f, 0.f, 0.f) - normal * normal.x;
        if (glm::length(tangent) < 0.0001f)
            tangent = glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent);
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = point.x / std::max(0.01f, textureScale);
        vertex.uv[1] = point.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(point.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<size_t>(endX - startX) *
        static_cast<size_t>(endZ - startZ) * 6u);
    const auto emitTriangle = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c)
    {
        glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        if (normal.y < 0.f)
        {
            std::swap(b, c);
            normal = -normal;
        }
        vertices.push_back(makeVertex(a, normal));
        vertices.push_back(makeVertex(b, normal));
        vertices.push_back(makeVertex(c, normal));
    };

    for (int z = startZ; z < endZ; ++z)
    {
        for (int x = startX; x < endX; ++x)
        {
            const float x0 = chunkOrigin.x + static_cast<float>(x) * cellSize;
            const float x1 = x0 + cellSize;
            const float z0 = chunkOrigin.z + static_cast<float>(z) * cellSize;
            const float z1 = z0 + cellSize;
            const glm::vec3 p00(x0, surfaceHeight(x0, z0), z0);
            const glm::vec3 p10(x1, surfaceHeight(x1, z0), z0);
            const glm::vec3 p01(x0, surfaceHeight(x0, z1), z1);
            const glm::vec3 p11(x1, surfaceHeight(x1, z1), z1);
            // Alternating the diagonal avoids a visible directional bias.
            if (((x + z) & 1) == 0)
            {
                emitTriangle(p00, p01, p11);
                emitTriangle(p00, p11, p10);
            }
            else
            {
                emitTriangle(p00, p01, p10);
                emitTriangle(p10, p01, p11);
            }
        }
    }
    return vertices;
}

std::vector<TerrainGen::Vertex>
TerrainGen::BuildHexagonVertices(int chunkX, int chunkZ,
    int patchX, int patchZ, const PerlinNoiseField& noise) const
{
    const int cells = std::clamp(horizontalCellsPerChunk, 2, 48);
    const int patchCount = std::clamp(patchesPerAxis, 1, std::min(8, cells));
    const int startX = patchX * cells / patchCount;
    const int endX = (patchX + 1) * cells / patchCount;
    const int startZ = patchZ * cells / patchCount;
    const int endZ = (patchZ + 1) * cells / patchCount;
    const float size = std::max(1.f, chunkSize);
    const float cellSize = size / static_cast<float>(cells);
    const float rootThree = std::sqrt(3.f);
    const float radius = cellSize / rootThree;
    const float columnSpacing = radius * 1.5f;
    const float rowSpacing = cellSize;
    const glm::vec3 chunkOrigin(
        static_cast<float>(chunkX) * size, 0.f,
        static_cast<float>(chunkZ) * size);
    const glm::vec3 patchOrigin(
        static_cast<float>(startX) * cellSize, 0.f,
        static_cast<float>(startZ) * cellSize);
    const float minimumX = chunkOrigin.x + static_cast<float>(startX) * cellSize;
    const float maximumX = chunkOrigin.x + static_cast<float>(endX) * cellSize;
    const float minimumZ = chunkOrigin.z + static_cast<float>(startZ) * cellSize;
    const float maximumZ = chunkOrigin.z + static_cast<float>(endZ) * cellSize;

    const auto makeVertex = [&](const glm::vec3& point, const glm::vec3& normal)
    {
        Vertex vertex{};
        const glm::vec3 local = point - chunkOrigin - patchOrigin;
        glm::vec3 tangent = std::abs(normal.x) < 0.9f
            ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        tangent = glm::normalize(tangent - normal * glm::dot(tangent, normal));
        vertex.pos[0] = local.x; vertex.pos[1] = local.y; vertex.pos[2] = local.z;
        vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
        vertex.uv[0] = point.x / std::max(0.01f, textureScale);
        vertex.uv[1] = point.z / std::max(0.01f, textureScale);
        vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z; vertex.tangent[3] = 1.f;
        const glm::vec3 color = ColorForHeight(point.y);
        vertex.color[0] = color.r; vertex.color[1] = color.g;
        vertex.color[2] = color.b; vertex.color[3] = 1.f;
        return vertex;
    };
    std::vector<Vertex> vertices;
    const auto emitTriangle = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c,
        const glm::vec3& desiredNormal)
    {
        if (glm::dot(glm::cross(b - a, c - a), desiredNormal) < 0.f)
            std::swap(b, c);
        vertices.push_back(makeVertex(a, desiredNormal));
        vertices.push_back(makeVertex(b, desiredNormal));
        vertices.push_back(makeVertex(c, desiredNormal));
    };
    const auto emitWall = [&](glm::vec3 a, glm::vec3 b, float bottom,
        const glm::vec3& normal)
    {
        const glm::vec3 lowA(a.x, bottom, a.z);
        const glm::vec3 lowB(b.x, bottom, b.z);
        emitTriangle(lowA, lowB, b, normal);
        emitTriangle(lowA, b, a, normal);
    };

    const int firstColumn = static_cast<int>(std::ceil(minimumX / columnSpacing));
    const int lastColumn = static_cast<int>(std::ceil(maximumX / columnSpacing));
    for (int column = firstColumn; column < lastColumn; ++column)
    {
        // Hex vertices live on an integer half-radius/half-row lattice. This
        // makes a corner shared across a jagged chunk boundary bit-identical,
        // regardless of which hex computes it.
        const auto latticePoint = [&](int xUnits, int zUnits)
        {
            return glm::vec2(
                static_cast<float>(static_cast<double>(xUnits) * radius * 0.5),
                static_cast<float>(static_cast<double>(zUnits) * cellSize * 0.5));
        };
        const float centerX = latticePoint(column * 3, 0).x;
        if (centerX < minimumX || centerX >= maximumX)
            continue;
        const int columnParity = column & 1;
        const float rowOffset = columnParity != 0 ? rowSpacing * 0.5f : 0.f;
        const int firstRow = static_cast<int>(std::ceil(
            (minimumZ - rowOffset) / rowSpacing));
        const int lastRow = static_cast<int>(std::ceil(
            (maximumZ - rowOffset) / rowSpacing));
        for (int row = firstRow; row < lastRow; ++row)
        {
            const glm::vec2 centerPoint = latticePoint(column * 3,
                row * 2 + columnParity);
            const float centerZ = centerPoint.y;
            if (centerZ < minimumZ || centerZ >= maximumZ)
                continue;
            const float height = SteppedHeight(centerX, centerZ, noise);
            const glm::vec3 center(centerX, height, centerZ);
            std::array<glm::vec3, 6> corners{};
            static constexpr int cornerOffsets[6][2] = {
                { 2, 0 }, { 1, 1 }, { -1, 1 },
                { -2, 0 }, { -1, -1 }, { 1, -1 }
            };
            for (int side = 0; side < 6; ++side)
            {
                const glm::vec2 point = latticePoint(
                    column * 3 + cornerOffsets[side][0],
                    row * 2 + columnParity + cornerOffsets[side][1]);
                corners[side] = { point.x, height, point.y };
            }
            static constexpr int evenNeighborOffsets[6][2] = {
                { 1, 0 }, { 0, 1 }, { -1, 0 },
                { -1, -1 }, { 0, -1 }, { 1, -1 }
            };
            static constexpr int oddNeighborOffsets[6][2] = {
                { 1, 1 }, { 0, 1 }, { -1, 1 },
                { -1, 0 }, { 0, -1 }, { 1, 0 }
            };
            static constexpr glm::vec3 sideNormals[6] = {
                { 0.866025404f, 0.f, 0.5f }, { 0.f, 0.f, 1.f },
                { -0.866025404f, 0.f, 0.5f },
                { -0.866025404f, 0.f, -0.5f }, { 0.f, 0.f, -1.f },
                { 0.866025404f, 0.f, -0.5f }
            };
            for (int side = 0; side < 6; ++side)
            {
                const int next = (side + 1) % 6;
                emitTriangle(center, corners[side], corners[next],
                    { 0.f, 1.f, 0.f });
                const glm::vec3 normal = sideNormals[side];
                const int (*neighborOffsets)[2] = columnParity != 0
                    ? oddNeighborOffsets : evenNeighborOffsets;
                const int neighborColumn = column + neighborOffsets[side][0];
                const int neighborRow = row + neighborOffsets[side][1];
                const glm::vec2 neighborCenter = latticePoint(neighborColumn * 3,
                    neighborRow * 2 + (neighborColumn & 1));
                const float neighborHeight = SteppedHeight(
                    neighborCenter.x, neighborCenter.y, noise);
                if (height > neighborHeight + 0.0001f)
                    emitWall(corners[side], corners[next], neighborHeight, normal);
            }
        }
    }
    return vertices;
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
            chunkObject->Parent = Owner;
            Owner->Children.push_back(chunkObject);
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
        static_cast<float>(chunkX) * std::max(1.f, chunkSize), 0.f,
        static_cast<float>(chunkZ) * std::max(1.f, chunkSize) };

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
            mesh->SetTerrainGeometry(std::move(geometry), generateColliders);
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
    for (int z = m_viewerChunk.y - radius; z <= m_viewerChunk.y + radius; ++z)
    {
        for (int x = m_viewerChunk.x - radius; x <= m_viewerChunk.x + radius; ++x)
            BuildChunk(x, z);
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - generationStart).count();
    char status[160]{};
    std::snprintf(status, sizeof(status),
        "Generated %zu chunks in %.2f ms without starting the game.",
        m_chunks.size(), milliseconds);
    m_editorGenerationStatus = status;
    return true;
}

bool TerrainGen::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    char viewerName[256]{};
    char noiseName[256]{};
    std::snprintf(viewerName, sizeof(viewerName), "%s", viewerObjectName.c_str());
    std::snprintf(noiseName, sizeof(noiseName), "%s", noiseObjectName.c_str());
    if (ui.InputText("Viewer Object", viewerName, sizeof(viewerName)))
    {
        viewerObjectName = viewerName;
        changed = true;
    }
    if (ui.InputText("Noise Object", noiseName, sizeof(noiseName)))
    {
        noiseObjectName = noiseName;
        changed = true;
    }

    changed = ui.DragFloat("Chunk Size", &chunkSize, 0.5f, 1.f, 1000.f) || changed;
    changed = ui.SliderInt("View Radius (Chunks)", &viewRadiusInChunks, 0, 8) || changed;
    changed = ui.SliderInt("Launches Per Update", &maxChunkBuildsPerUpdate, 1, 16) || changed;
    changed = ui.SliderInt("Parallel Chunk Builds", &parallelChunkBuilds, 1, 16) || changed;
    changed = ui.SliderInt("Chunk Commits Per Update",
        &maxChunkCommitsPerUpdate, 1, 16) || changed;
    changed = ui.SliderInt("Chunk Unloads Per Update",
        &maxChunkUnloadsPerUpdate, 1, 32) || changed;
    changed = ui.Checkbox("Cache Unloaded Chunk Meshes",
        &cacheUnloadedChunkMeshes) || changed;
    if (cacheUnloadedChunkMeshes)
    {
        changed = ui.SliderInt("Unloaded Mesh Cache Capacity",
            &unloadedMeshCacheCapacity, 0, 1024) || changed;
    }
    changed = ui.SliderInt("Horizontal Cells", &horizontalCellsPerChunk, 2, 48) || changed;
    changed = ui.SliderInt("Vertical Cells", &verticalCells, 2, 48) || changed;
    changed = ui.SliderInt("Mesh Patches Per Axis", &patchesPerAxis, 1, 8) || changed;
    static const char* terrainShapes[] = {
        "Smooth Surface", "Cubes", "Triangles", "Hexagons"
    };
    changed = ui.Combo("Terrain Shape", &terrainShape, terrainShapes, 4) || changed;
    const TerrainShape shape = static_cast<TerrainShape>(terrainShape);
    if (shape == TerrainShape::Cubes || shape == TerrainShape::Hexagons)
    {
        changed = ui.DragFloat("Height Step", &heightStep,
            0.05f, 0.05f, 100.f) || changed;
        ui.DisabledLabel(shape == TerrainShape::Cubes
            ? "Square columns use horizontal tops and vertical walls."
            : "Hexagonal columns use six-sided tops and exposed walls.");
    }
    changed = ui.DragFloat("Vertical Size", &verticalSize, 0.5f, 1.f, 1000.f) || changed;
    changed = ui.DragFloat("Base Height", &baseHeight, 0.1f) || changed;
    changed = ui.DragFloat("Height Amplitude", &heightAmplitude, 0.1f, 0.f, 1000.f) || changed;
    if (shape == TerrainShape::SmoothSurface)
    {
        changed = ui.DragFloat("Cave Strength", &caveStrength, 0.05f, 0.f, 100.f) || changed;
        changed = ui.DragFloat("Cave Frequency Multiplier", &caveFrequencyMultiplier,
            0.05f, 0.01f, 100.f) || changed;
        changed = ui.DragFloat("Iso Level", &isoLevel, 0.05f) || changed;
    }
    changed = ui.DragFloat("Texture Scale", &textureScale, 0.1f, 0.01f, 1000.f) || changed;
    changed = ui.DragFloat("Low Height Maximum", &lowHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Low Height Color", &lowHeightColor.x) || changed;
    changed = ui.DragFloat("Middle Height Maximum", &middleHeightMaximum, 0.1f) || changed;
    changed = ui.ColorEdit3("Middle Height Color", &middleHeightColor.x) || changed;
    changed = ui.ColorEdit3("High Height Color", &highHeightColor.x) || changed;
    changed = ui.Checkbox("Generate Colliders", &generateColliders) || changed;

    char value[96]{};
    std::snprintf(value, sizeof(value), "%zu", m_chunks.size());
    ui.ValueLabel("Loaded Chunks", value);
    std::snprintf(value, sizeof(value), "%llu built / %llu unloaded",
        static_cast<unsigned long long>(m_totalChunksBuilt),
        static_cast<unsigned long long>(m_totalChunksUnloaded));
    ui.ValueLabel("Streaming Activity", value);
    std::snprintf(value, sizeof(value), "%.3f ms last / %.3f ms maximum",
        m_lastStreamingMilliseconds, m_maximumStreamingMilliseconds);
    ui.ValueLabel("Streaming CPU Time", value);
    std::snprintf(value, sizeof(value), "%zu cached / %llu hits / %llu misses",
        m_meshCache.size(), static_cast<unsigned long long>(m_meshCacheHits),
        static_cast<unsigned long long>(m_meshCacheMisses));
    ui.ValueLabel("Mesh Cache", value);
    std::snprintf(value, sizeof(value), "%zu queued / %zu generating",
        m_chunkQueue.size(), m_inFlightChunks.size());
    ui.ValueLabel("Parallel Build Queue", value);
    std::snprintf(value, sizeof(value), "%zu", m_pendingChunkUnloadCount);
    ui.ValueLabel("Pending Chunk Unloads", value);
    ui.Separator();
    if (ui.Button("Generate Terrain Now"))
    {
        GenerateTerrain();
        changed = true;
    }
    if (!m_editorGenerationStatus.empty())
        ui.DisabledLabel(m_editorGenerationStatus.c_str());
    return changed;
}
