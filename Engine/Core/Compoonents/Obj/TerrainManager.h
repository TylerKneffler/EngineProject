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
    std::string viewerObjectName;
    // When set, procedural brush edits are stored as independent sparse files.
    std::string cellDataDirectory;
    bool HasCell(int x, int y) const;
    std::vector<Cell> GetCells() const;
    size_t GetCellCount() const { return m_cells.size() + m_proceduralCells.size(); }
    size_t GetLoadedCellCount() const { return m_loaded.size(); }
    size_t GetRenderBatchCount() const { return m_renderBatches.size(); }
    uint64_t GetRenderBatchUploadCount() const { return m_renderBatchUploadCount; }
    size_t GetPreparingCellCount() const { return m_preparingCells.size(); }
    size_t GetCacheHitCount() const { return m_cacheHits; }
    Terrain* GetLoadedCell(int x, int y) const;
    bool AddCell(int x, int y);
    bool AddProceduralCell(int x, int y);
    void SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder);
    void SetStaticCache(std::string directory, uint64_t signature);
    std::string GetCacheFilePath(int x, int y) const;
    // Bake a deterministic cell without retaining it in the scene or GPU memory.
    bool BakeProceduralCell(int x, int y, bool force = false);
    size_t GetStoredTerrainCount() const { return m_cells.size(); }
    bool RemoveCell(int x, int y);
    bool ApplyBrush(double x, double z, float radius, float strength,
        Terrain::Brush brush, int layer = 0);
    void SyncLoadedCells(int centerX, int centerY);
    void UpdateStreaming(int centerX, int centerY);
    void UpdateEditorStreaming(const glm::dvec3& worldPosition);
    void Update() override;
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
    };
    std::map<Cell, RenderBatch> m_renderBatches;
    uint64_t m_renderBatchUploadCount = 0;
    IGraphicsProvider* m_provider = nullptr;
    struct PrepareTask
    {
        Cell cell;
        uint64_t revision = 0, cacheSignature = 0;
        int cellsX = 0, cellsY = 0, lodStride = 1;
        float cellWidth = 1.f, cellDepth = 1.f;
        bool hasSaved = false, edited = false;
        JsonValue saved;
        std::string cachePath, editPath;
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
    void LoadCell(Cell cell, int lodStride = 1);
    void StartPrepareWorker();
    void InvalidatePrepared();
    void PrepareLoop();
    int CommitPrepared(int centerX, int centerY, int radius, int uploadBudget);
    std::string GetEditFilePath(Cell cell) const;
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
