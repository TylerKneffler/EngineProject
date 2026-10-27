#pragma once
#include "Core/Component.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include <map>
#include <set>
#include <functional>
#include <utility>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>

namespace Engine::Components
{
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
    int farRadius = -1;
    int lod2Radius = 8;
    int lod4Radius = 16;
    int maxLodChangesPerUpdate = 4;
    int maxLoadsPerUpdate = 2;
    int maxUnloadsPerUpdate = 4;
    bool asyncStreaming = false;
    int maxPreparedCells = 4;
    int maxUploadsPerUpdate = 2;
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
    size_t GetStoredTerrainCount() const { return m_cells.size(); }
    bool RemoveCell(int x, int y);
    bool ApplyBrush(double x, double z, float radius, float strength,
        Terrain::Brush brush, int layer = 0);
    void SyncLoadedCells(int centerX, int centerY);
    void UpdateStreaming(int centerX, int centerY);
    void UpdateStreamingAtPosition(const glm::dvec3& worldPosition);
    void UpdateEditorStreaming(const glm::dvec3& worldPosition);
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
    void TrimMemoryBudget(int centerX, int centerY);
    bool ReserveCacheWrite(const std::string& path, const Terrain& terrain);
    bool SaveEdit(Cell cell, const Terrain& terrain) const;
    bool LoadEdit(Cell cell, Terrain& terrain) const;
};
}
