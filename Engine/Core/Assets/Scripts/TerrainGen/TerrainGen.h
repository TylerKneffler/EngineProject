#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include "Core/Model/MeshData.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class PerlinNoiseField;
class TerrainChunk;

// Streams procedurally generated terrain chunks around a named viewer. The
// output topology is selectable without changing the streaming pipeline.
class TerrainGen final : public Engine::Core::Script
{
public:
    enum class TerrainShape : int
    {
        SmoothSurface = 0,
        Cubes = 1,
        Triangles = 2,
        Hexagons = 3
    };

    TerrainGen();
    ~TerrainGen() override;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | References")
    std::string viewerObjectName = "TerrainGen Camera";

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | References")
    std::string noiseObjectName;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1")
    float chunkSize = 16.f;

    // Logical chunk origin represented by local coordinate (0, 0). Keeping
    // rendered/physical transforms near zero prevents large-world float loss.
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming")
    int worldOriginChunkX = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming")
    int worldOriginChunkZ = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "0", ClampMax = "8")
    int viewRadiusInChunks = 1;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1", ClampMax = "16")
    int maxChunkBuildsPerUpdate = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1", ClampMax = "16")
    int parallelChunkBuilds = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1", ClampMax = "16")
    int maxChunkCommitsPerUpdate = 4;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "1", ClampMax = "32")
    int maxChunkUnloadsPerUpdate = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming")
    bool cacheUnloadedChunkMeshes = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Streaming", ClampMin = "0", ClampMax = "1024")
    int unloadedMeshCacheCapacity = 128;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "2", ClampMax = "48")
    int horizontalCellsPerChunk = 12;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "2", ClampMax = "48")
    int verticalCells = 12;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Resolution", ClampMin = "1", ClampMax = "8")
    int patchesPerAxis = 2;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Geometry")
    int terrainShape = static_cast<int>(TerrainShape::SmoothSurface);

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Geometry", ClampMin = "0.05")
    float heightStep = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "1")
    float verticalSize = 18.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape")
    float baseHeight = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0")
    float heightAmplitude = 6.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0")
    float caveStrength = 1.4f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape", ClampMin = "0.01")
    float caveFrequencyMultiplier = 2.35f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Shape")
    float isoLevel = 0.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Rendering", ClampMin = "0.01")
    float textureScale = 12.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors")
    float lowHeightMaximum = -1.5f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors")
    float middleHeightMaximum = 2.5f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors")
    glm::vec3 lowHeightColor { 0.16f, 0.28f, 0.48f };

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors")
    glm::vec3 middleHeightColor { 0.2f, 0.52f, 0.18f };

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors")
    glm::vec3 highHeightColor { 0.62f, 0.58f, 0.48f };

    // Fraction of the low-to-high threshold range blended on either side of
    // each boundary. A narrow default keeps elevation bands readable while
    // retaining a smooth transition on rounded terrain.
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Height Colors", Range = "0, 0.5")
    float heightColorBlend = 0.08f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Physics")
    bool generateColliders = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Physics", ClampMin = "0", ClampMax = "8")
    int collisionRadiusInChunks = 0;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain | Physics", ClampMin = "1", ClampMax = "16")
    int maxColliderActivationsPerUpdate = 1;

    void Start() override;
    void Update() override;
    void OnDestroy() override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

    // Rebuilds the configured terrain ring immediately. This is also exposed
    // by the custom editor UI and does not require the scene runtime to start.
    bool GenerateTerrain();

    // Removes generated chunks and invalidates every cached or queued mesh.
    // Noise/settings remain intact so GenerateTerrain can start from a clean slate.
    void ClearTerrain();

    std::size_t GetLoadedChunkCount() const { return m_chunks.size(); }
    uint64_t GetTotalChunksBuilt() const { return m_totalChunksBuilt; }
    uint64_t GetTotalChunksUnloaded() const { return m_totalChunksUnloaded; }
    double GetLastStreamingMilliseconds() const { return m_lastStreamingMilliseconds; }
    double GetMaximumStreamingMilliseconds() const { return m_maximumStreamingMilliseconds; }
    uint64_t GetMeshCacheHits() const { return m_meshCacheHits; }
    uint64_t GetMeshCacheMisses() const { return m_meshCacheMisses; }
    std::size_t GetCachedChunkCount() const { return m_meshCache.size(); }
    std::size_t GetQueuedChunkCount() const { return m_chunkQueue.size(); }
    std::size_t GetInFlightChunkCount() const { return m_inFlightChunks.size(); }
    std::size_t GetPendingChunkUnloadCount() const { return m_pendingChunkUnloadCount; }
    bool IsChunkLoaded(int x, int z) const
    {
        return m_chunks.find(ChunkKey(x, z)) != m_chunks.end();
    }

private:
    using Vertex = Engine::Model::AnimationVertex;
    using TerrainMeshData = Engine::Model::TerrainMeshData;

    struct CachedPatchMesh
    {
        int patchX = 0;
        int patchZ = 0;
        TerrainMeshData geometry;
    };
    struct CachedChunkMesh
    {
        std::vector<CachedPatchMesh> patches;
        uint64_t lastUse = 0;
    };
    struct GenerationSnapshot
    {
        float chunkSize = 16.f;
        int horizontalCells = 12;
        int verticalCells = 12;
        int patchesPerAxis = 2;
        int terrainShape = 0;
        float heightStep = 1.f;
        float verticalSize = 18.f;
        float baseHeight = 0.f;
        float heightAmplitude = 6.f;
        float caveStrength = 1.4f;
        float caveFrequencyMultiplier = 2.35f;
        float isoLevel = 0.f;
        float textureScale = 12.f;
        float lowHeightMaximum = -1.5f;
        float middleHeightMaximum = 2.5f;
        glm::vec3 lowHeightColor{};
        glm::vec3 middleHeightColor{};
        glm::vec3 highHeightColor{};
        float heightColorBlend = 0.08f;
        int noiseSeed = 1337;
        float noiseFrequency = 0.045f;
        int noiseOctaves = 4;
        float noiseLacunarity = 2.f;
        float noisePersistence = 0.5f;
        glm::vec3 noiseOffset{};
        uint64_t configurationHash = 0;
    };
    struct GeneratedChunkMesh
    {
        int x = 0;
        int z = 0;
        uint64_t configurationHash = 0;
        uint64_t generationEpoch = 0;
        std::vector<CachedPatchMesh> patches;
    };
    struct QueuedChunk
    {
        int x = 0;
        int z = 0;
        int64_t distanceSquared = 0;
    };
    struct InFlightChunk
    {
        int x = 0;
        int z = 0;
    };
    struct GenerationJob
    {
        GenerationSnapshot snapshot;
        int x = 0;
        int z = 0;
        uint64_t generationEpoch = 0;
    };

    static int64_t ChunkKey(int x, int z);
    glm::ivec2 ViewerChunk() const;
    PerlinNoiseField* ResolveNoise() const;
    void RefreshChunks();
    void RefreshColliderActivation();
    void BuildChunk(int chunkX, int chunkZ,
        std::vector<CachedPatchMesh>* preparedPatches = nullptr);
    GenerationSnapshot CaptureGenerationSnapshot(
        const PerlinNoiseField& noise) const;
    static GeneratedChunkMesh GenerateChunkMesh(
        const GenerationSnapshot& snapshot, int chunkX, int chunkZ);
    void EnsureWorkerPool(std::size_t workerCount);
    void StopWorkerPool();
    void CancelPendingGeneration();
    void WorkerLoop();
    void HarvestCompletedChunks();
    int CommitCompletedChunks(int budget);
    bool IsChunkDesired(int x, int z) const;
    std::vector<Vertex> BuildPatchVertices(int chunkX, int chunkZ,
        int patchX, int patchZ, const PerlinNoiseField& noise) const;
    std::vector<Vertex> BuildSmoothSurfaceVertices(int chunkX, int chunkZ,
        int patchX, int patchZ, const PerlinNoiseField& noise) const;
    std::vector<Vertex> BuildCubeVertices(int chunkX, int chunkZ,
        int patchX, int patchZ, const PerlinNoiseField& noise) const;
    std::vector<Vertex> BuildTriangleVertices(int chunkX, int chunkZ,
        int patchX, int patchZ, const PerlinNoiseField& noise) const;
    std::vector<Vertex> BuildHexagonVertices(int chunkX, int chunkZ,
        int patchX, int patchZ, const PerlinNoiseField& noise) const;
    float Density(const glm::vec3& terrainPosition,
        const PerlinNoiseField& noise) const;
    float SteppedHeight(double worldX, double worldZ,
        const PerlinNoiseField& noise) const;
    glm::vec3 ColorForHeight(float height) const;
    glm::vec3 SmoothColorForHeight(float height) const;
    uint64_t MeshConfigurationHash(const PerlinNoiseField& noise) const;
    void CacheChunkMesh(int64_t key, Engine::Core::Object& chunkObject);
    void TrimMeshCache();

    std::unordered_map<int64_t, Engine::Core::Object*> m_chunks;
    // Detached, disabled chunk hierarchies retained for normal edge-to-edge
    // streaming. Reusing them avoids destroying live GPU resources whenever
    // the viewer crosses a chunk boundary.
    std::vector<Engine::Core::Object*> m_chunkObjectPool;
    glm::ivec2 m_viewerChunk { 0 };
    bool m_hasViewerChunk = false;
    std::string m_editorGenerationStatus;
    uint64_t m_totalChunksBuilt = 0;
    uint64_t m_totalChunksUnloaded = 0;
    double m_lastStreamingMilliseconds = 0.0;
    double m_maximumStreamingMilliseconds = 0.0;
    std::unordered_map<int64_t, CachedChunkMesh> m_meshCache;
    uint64_t m_meshCacheClock = 0;
    uint64_t m_meshConfigurationHash = 0;
    uint64_t m_meshCacheHits = 0;
    uint64_t m_meshCacheMisses = 0;
    std::vector<QueuedChunk> m_chunkQueue;
    std::unordered_map<int64_t, InFlightChunk> m_inFlightChunks;
    std::unordered_map<int64_t, GeneratedChunkMesh> m_readyChunks;
    std::vector<std::thread> m_workerThreads;
    std::deque<GenerationJob> m_generationJobs;
    std::deque<GeneratedChunkMesh> m_completedChunks;
    std::mutex m_generationMutex;
    std::condition_variable m_generationCondition;
    bool m_stopWorkers = false;
    uint64_t m_generationEpoch = 1;
    std::size_t m_pendingChunkUnloadCount = 0;
};
