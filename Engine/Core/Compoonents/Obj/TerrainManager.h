#pragma once
#include "Core/Component.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainHeightArchive.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include <map>
#include <array>
#include <set>
#include <functional>
#include <utility>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>
#include <memory>
#include <filesystem>

namespace Engine::Components
{
class Camera;
class TerrainManager final : public Engine::Core::Component
{
public:
    using Cell = std::pair<int, int>;
    TerrainManager();
    ~TerrainManager() override;
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    int loadedRadius = 2;
    // Scene view streams only editable, full-resolution cells around its camera.
    int editorRadius = 6;
    // -1 follows loadedRadius for older scenes; larger values stream coarse cells.
    int farRadius = 8;
    int lod2Radius = 8;
    int lod4Radius = 16;
    int maxLodChangesPerUpdate = 4;
    int maxLoadsPerUpdate = 2;
    int maxUnloadsPerUpdate = 4;
    bool asyncStreaming = false;
    int maxPreparedCells = 4;
    int maxUploadsPerUpdate = 2;
    float maxFarRebuildMilliseconds = 4.f;
    float farTileScreenErrorPixels = 3.f;
    float farTileLodHysteresis = .2f;
    bool batchFarCells = true;
    // Optional fast directional diffuse shading for static play-mode terrain.
    bool fastRuntimeLighting = false;
    // Compare stochastic tile variation with the low-cost world-space UV warp.
    bool stochasticTerrainVariation = false;
    // Measured lowest-mip layer colors can replace distant detail in the
    // resident terrain window. Keep opt-in until its GPU cost is lower.
    bool terrainMacroColorEnabled = false;
    // Show a world-space metric ruler and cell boundaries on every terrain mesh.
    bool measurementGrid = false;
    // Adds static mesh colliders to streamed terrain cells for physics actors.
    bool generateColliders = false;
    // Keep terrain collision active only near the viewer.
    bool streamCollidersByDistance = true;
    // Terrain mesh and cell data only; shared textures and renderer targets are excluded.
    int cpuBudgetMiB = 512;
    int gpuBudgetMiB = 256;
    int cacheBudgetMiB = 1024;
    struct MemoryUsage
    {
        uint64_t cpuBytes = 0, gpuBytes = 0, preparedBytes = 0;
        uint64_t cacheBytes = 0;
    };
    MemoryUsage GetMemoryUsage() const;
    uint64_t GetTerrainTextureMemoryBytes() const;
    std::string viewerObjectName;
    // When set, procedural brush edits are stored as independent sparse files.
    std::string cellDataDirectory;
    // Persistent, distributable cells written by the inspector bake action.
    std::string runtimeBakeDirectory;
    // Require the authored bake in play mode; editor source data remains available for rebaking.
    bool requireRuntimeBake = false;
    bool HasCell(int x, int y) const;
    std::vector<Cell> GetCells() const;
    size_t GetCellCount() const { return m_cells.size() + m_proceduralCells.size(); }
    size_t GetLoadedCellCount() const { return m_loaded.size(); }
    size_t GetRenderBatchCount() const { return m_renderBatches.size(); }
    uint64_t GetRenderBatchUploadCount() const { return m_renderBatchUploadCount; }
    double GetRenderBatchRefreshMilliseconds() const
    { return m_renderBatchRefreshMilliseconds; }
    double GetRenderBatchBuildMilliseconds() const
    { return m_renderBatchBuildMilliseconds; }
    size_t GetPreparingCellCount() const { return m_preparingCells.size(); }
    size_t GetCacheHitCount() const { return m_cacheHits; }
    Terrain* GetLoadedCell(int x, int y) const;
    bool AddCell(int x, int y);
    bool AddProceduralCell(int x, int y);
    void SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder);
    void SetStaticCache(std::string directory, uint64_t signature);
    bool ReadBakedCellHeights(Cell cell, std::vector<float>& heights,
        std::vector<float>* weights = nullptr,
        uint32_t* weightLayerCount = nullptr) const;
    std::string GetCacheFilePath(int x, int y) const;
    // Bake a deterministic cell without retaining it in the scene or GPU memory.
    bool BakeProceduralCell(int x, int y, bool force = false);
    bool BakeRuntimeTerrain(bool reuseExistingCells = false);
    bool HasCompleteRuntimeBake() const;
    struct FarTerrainPalette
    {
        std::array<glm::vec3, TerrainHeightArchive::LayerCount> farColors{};
        std::array<glm::vec3, 6> nearColors{};
        std::array<int, TerrainHeightArchive::LayerCount> archiveToNear{};
    };
    // Distant visual tiles share this manager's cell grid and scene lifetime.
    void ConfigureFarTerrain(int halfExtentCells, std::vector<float> heights,
        std::vector<TerrainHeightArchive::FarPatch> patches,
        std::string heightArchivePath, uint64_t sourceSignature,
        FarTerrainPalette palette);
    void ClearFarTerrain();
    void SetFarTerrainEnabled(bool enabled);
    void UpdateFarTerrain(int centerX, int centerY,
        const Camera* camera = nullptr, int viewportHeight = 1080,
        float aspect = 16.f / 9.f);
    std::vector<Mesh*> GetFarTileMeshes() const;
    size_t GetVisibleFarTileCount() const;
    size_t GetVisibleFarTileCountForStride(int stride) const;
    size_t GetDesiredFarTileCount() const;
    size_t GetBuiltFarTileCount() const;
    size_t GetPreparingFarTileCount() const;
    size_t GetQueuedFarTileCount() const;
    size_t GetPreparedFarTileCount() const;
    size_t GetVisibleFarTileVertexCount() const;
    uint64_t GetFarTileUploadCount() const { return m_farTileUploadCount; }
    uint64_t GetFarTileCacheHits() const { return m_farTileCacheHits; }
    uint64_t GetFarTileUploadedBytes() const { return m_farTileUploadedBytes; }
    double GetFarTileRebuildMilliseconds() const
    { return m_farTileRebuildMilliseconds; }
    size_t GetStoredTerrainCount() const { return m_cells.size(); }
    std::vector<Cell> GetEditedProceduralCells() const
    { return {m_editedProcedural.begin(), m_editedProcedural.end()}; }
    bool RemoveCell(int x, int y);
    bool ApplyBrush(double x, double z, float radius, float strength,
        Terrain::Brush brush, int layer = 0);
    void SyncLoadedCells(int centerX, int centerY);
    void UpdateStreaming(int centerX, int centerY);
    void UpdateStreamingAtPosition(const glm::dvec3& worldPosition);
    void UpdateEditorStreaming(const glm::dvec3& worldPosition,
        const Camera* camera = nullptr, int viewportHeight = 1080,
        float aspect = 16.f / 9.f);
    bool TryGetHeightAtWorldPosition(const glm::dvec3& worldPosition,
        double& height) const;
    void Update() override;
    void Start() override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    std::map<Cell, JsonValue> m_cells;
    std::set<Cell> m_proceduralCells;
    std::set<Cell> m_editedProcedural;
    mutable std::set<Cell> m_dirtyProceduralEdits;
    std::function<void(Cell, Terrain&)> m_proceduralBuilder;
    std::string m_cacheDirectory;
    uint64_t m_cacheSignature = 0;
    size_t m_cacheHits = 0;
    std::atomic<uint64_t> m_cacheReservedBytes{0};
    std::map<Cell, Terrain*> m_loaded;
    struct RenderBatch
    {
        Engine::Core::Object* object = nullptr;
        uint64_t revision = 0;
        std::set<Cell> members;
        std::map<Cell, int> lodStrides;
        std::map<Cell, uint64_t> geometryRevisions;
        std::map<Cell, uint64_t> transformRevisions;
    };
    std::map<Cell, RenderBatch> m_renderBatches;
    struct FarTile
    {
        int startX = 0, startY = 0, endX = 0, endY = 0;
        std::vector<size_t> patchIndices;
        std::map<size_t, size_t> patchVertexOffsets;
        std::set<size_t> dirtyPatchIndices;
        std::vector<size_t> children;
        size_t parentIndex = static_cast<size_t>(-1);
        int lodStride = 1;
        bool desired = false;
        bool outOfRange = false;
        bool coarseSelected = false;
        float maxHeightError = 0.f;
        float minHeight = 0.f, maxHeight = 0.f;
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        int builtCenterX = 0, builtCenterY = 0, builtNearRadius = 0;
        std::vector<uint8_t> builtVisibility;
        bool built = false, hasGeometry = false;
        bool preparing = false;
    };
    int m_farHalfExtentCells = 0;
    std::vector<float> m_farHeights;
    std::shared_ptr<const std::vector<float>> m_farHeightsSnapshot;
    std::vector<TerrainHeightArchive::FarPatch> m_farPatches;
    std::set<Cell> m_farPatchCells;
    std::vector<FarTile> m_farTiles;
    void RefreshFarTerrainEditedCells(const std::set<Cell>& changedCells);
    std::string m_farHeightArchivePath;
    FarTerrainPalette m_farPalette;
    uint64_t m_farSourceSignature = 0;
    uint64_t m_farTileUploadCount = 0;
    uint64_t m_farTileCacheHits = 0;
    uint64_t m_farTileUploadedBytes = 0;
    double m_farTileRebuildMilliseconds = 0.0;
    bool m_farEnabled = false;
    uint64_t m_uploadBudgetFrame = UINT64_MAX;
    size_t m_nearFrameUploads = 0;
    size_t m_farFrameUploads = 0;
    struct FarPrepareTask
    {
        size_t tileIndex = 0;
        uint64_t revision = 0, fingerprint = 0;
        int startX = 0, startY = 0, endX = 0, endY = 0;
        int lodStride = 1, side = 0, halfExtent = 0;
        float spanX = 1.f, spanZ = 1.f;
        FarTerrainPalette palette;
        std::shared_ptr<const std::vector<float>> heights;
        int cellsX = 1, cellsY = 1;
        float cellWidth = 1.f, cellDepth = 1.f;
        std::set<Cell> hiddenCells;
        struct SeamSurface
        {
            std::vector<float> heights, weights;
            uint32_t layerCount = 0;
        };
        std::map<Cell, SeamSurface> seams;
        std::vector<std::pair<size_t, TerrainHeightArchive::FarPatch>> patches;
        std::filesystem::path cachePath;
    };
    struct PreparedFarTile
    {
        size_t tileIndex = 0;
        uint64_t revision = 0;
        bool cacheHit = false;
        bool success = false;
        double buildMilliseconds = 0.0;
        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32_t> indices;
        std::map<size_t, size_t> patchVertexOffsets;
        std::vector<uint8_t> builtVisibility;
    };
    std::thread m_farWorker;
    mutable std::mutex m_farPrepareMutex;
    std::condition_variable m_farPrepareWake;
    std::deque<FarPrepareTask> m_farPrepareQueue;
    std::deque<PreparedFarTile> m_preparedFarTiles;
    uint64_t m_farPrepareRevision = 1;
    bool m_stopFarWorker = false;
    void StartFarPrepareWorker();
    void FarPrepareLoop();
    uint64_t m_renderBatchUploadCount = 0;
    double m_renderBatchRefreshMilliseconds = 0.0;
    double m_renderBatchBuildMilliseconds = 0.0;
    bool m_renderBatchRefreshPending = false;
    std::string m_bakeStatus;
    bool m_runtimeBakeActive = false;
    struct BakeEntry { uint64_t offset = 0, size = 0; };
    std::map<Cell, BakeEntry> m_bakeIndex;
    IGraphicsProvider* m_provider = nullptr;
    struct PrepareTask
    {
        Cell cell;
        uint64_t revision = 0, cacheSignature = 0;
        int cellsX = 0, cellsY = 0, lodStride = 1;
        float cellWidth = 1.f, cellDepth = 1.f;
        bool hasSaved = false, edited = false;
        bool requireBake = false;
        JsonValue saved;
        std::string cachePath, editPath;
        std::string bakePath;
        uint64_t bakeOffset = 0, bakeSize = 0;
        std::function<void(Cell, Terrain&)> builder;
    };
    struct PreparedCell
    {
        Cell cell;
        uint64_t revision = 0;
        bool cacheHit = false;
        Terrain::Prepared terrain;
    };
    std::thread m_prepareWorker;
    mutable std::mutex m_prepareMutex;
    std::condition_variable m_prepareWake;
    std::deque<PrepareTask> m_prepareQueue;
    std::deque<PreparedCell> m_preparedCells;
    std::map<Cell, uint64_t> m_preparingCells;
    uint64_t m_prepareRevision = 1;
    bool m_stopPrepareWorker = false;
    bool m_haveLastCenter = false;
    bool m_editorStreaming = false;
    Cell m_lastCenter{0, 0};
    std::optional<Cell> m_prefetchCenter;
    void LoadCell(Cell cell, int lodStride = 1);
    void StartPrepareWorker();
    void InvalidatePrepared();
    void PrepareLoop();
    int CommitPrepared(int centerX, int centerY, int radius, int uploadBudget);
    std::string GetEditFilePath(Cell cell) const;
    std::string GetRuntimeBakeFilePath(Cell cell) const;
    std::string GetRuntimeBakeArchivePath() const;
    bool ReadRuntimeBakeIndex(const std::string& path,
        std::map<Cell, BakeEntry>& entries) const;
    int DesiredLodStride(Cell cell, int centerX, int centerY) const;
    void StreamCells(int centerX, int centerY);
    void UnloadCell(Cell cell);
    void Stitch(Cell cell);
    void RefreshRenderBatches(int uploadBudget, int centerX, int centerY,
        int radius);
    void RefreshColliderActivation(int centerX, int centerY);
    void TrimMemoryBudget(int centerX, int centerY);
    bool ReserveCacheWrite(const std::string& path, const Terrain& terrain);
    bool SaveEdit(Cell cell, const Terrain& terrain) const;
    bool LoadEdit(Cell cell, Terrain& terrain) const;
    void ConfigureCellPhysics(Engine::Core::Object& object) const;
};
}
