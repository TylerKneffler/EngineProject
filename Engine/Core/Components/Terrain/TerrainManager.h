#pragma once
#include "Core/Component.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include <climits>
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
#include <chrono>
#include <vector>
#include <future>
#include <string>

namespace Engine::Scene { class Scene; }

namespace Engine::Components
{
class Camera;
class TerrainObjectLayer;
class VolumeTerrain;
class TerrainManager final : public Engine::Core::Component
{
public:
    using Cell = std::pair<int, int>;
    TerrainManager();
    ~TerrainManager() override;
    void FenceBackgroundWork() override;
    int cellsX = 32, cellsY = 32;
    float cellWidth = 1.f, cellDepth = 1.f;
    int loadedRadius = 1;
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
    bool prefetchAdjacentCells = true;
    // Oak Meadow maps: synchronous loads stream asynchronously too. The
    // meadow's content (oak forest, meadow grass) comes from its scene
    // script (TerrainPerformanceDriver) through the layer generator/setup hooks.
    bool proceduralOakMeadow = false;
    int maxPreparedCells = 4;
    int maxUploadsPerUpdate = 2;
    float maxFarRebuildMilliseconds = 4.f;
    float farTileScreenErrorPixels = 3.f;
    // Far objects: beyond the loaded cells, out to this radius (cells), the
    // object layers' placements are drawn with only the last level of their
    // LOD chains (tree impostors), batched into blocks of
    // farObjectBlockCells x farObjectBlockCells cells. Only placements whose
    // asset has a LOD chain take part. 0 disables.
    int farObjectRadius = 0;
    // Main-thread time per update for installing streamed cells into the
    // scene. At least one cell commits each update; more only within this.
    float maxCommitMilliseconds = 2.f;
    int farObjectBlockCells = 8;
    // Near object blocks: the prefab placements of the loaded cells are drawn
    // by one layer per nearObjectBlockCells x nearObjectBlockCells cells (and
    // layer name) instead of one per cell. A prefab batch is a scene object
    // per mesh part and LOD level, and the frame's CPU cost follows how many
    // exist (~2 ms per 1,000 on the landscape), while the GPU culls their
    // placements one by one either way. The cells' own layers keep their
    // placements and colliders. 1 (or less) keeps one batch set per cell.
    // Game streaming only; the editor always draws per cell.
    int nearObjectBlockCells = 1;
    // Loaded cells keep their mesh in the compact terrain vertex format, with
    // a paint map for the layer weights (Terrain::compactMesh): 48 bytes a
    // vertex instead of 264. Game streaming; the editor keeps the full format.
    bool compactCellVertices = false;
    // compactCellVertices for game streaming; ENGINE_COMPACT_TERRAIN=0/1
    // overrides it for A/B runs.
    bool UsesCompactCellVertices() const;
    // Volume terrain (stage 3, .md/todo.md): loaded cells are drawn and
    // collided as density volumes built from them (VolumeTerrain fromTerrain:
    // their heights and volume edits), shaded with their paint and
    // `volumeSteepLayer` on steep faces. The cell's Terrain stays for heights,
    // paint and placement; its own mesh and grid collider go.
    // Far tiles keep the height field. ENGINE_VOLUME_TERRAIN=0/1 overrides.
    bool volumeCells = false;
    int volumeSteepLayer = 4;
    // Metres between volume samples. The heights are a 4 m grid, so 2 m
    // keeps their detail and costs an eighth of 1 m (and ~a quarter of
    // the triangles).
    float volumeSampleSpacing = 2.f;
    bool UsesVolumeCells() const;
    // Volume edits (caves, arches; TerrainVolumeEdit), kept per cell in
    // <cellDataDirectory>/<x>_<z>.volumeedit in the cell's space, read when
    // the cell's volume is built (with its neighbours', which may reach
    // across). They show on volume cells only: far tiles and height queries
    // (placement, grass) keep the height field.
    const std::vector<TerrainVolumeEdit>& GetVolumeEdits(Cell cell) const;
    // Adds an edit given in the grid's space to the cell holding its centre,
    // saves that cell's file, and rebuilds the loaded volume cells it
    // reaches (their current chunks draw until the new ones are ready).
    // False when the file cannot be written; an edit a cell or more across
    // is refused.
    bool AddVolumeEdit(const TerrainVolumeEdit& edit);
    std::string GetVolumeEditFilePath(Cell cell) const;
    // Project layer for far object blocks (see Rendering::DetailBudget).
    std::string farObjectProjectLayer = "Trees";
    // Far forest clusters: far blocks beyond farClusterDistance (metres from
    // the viewer) group their trees on a farClusterSize grid, and each square
    // with at least farClusterMinimumTrees becomes one cluster impostor: a
    // baked stand of trees (Tools/generate_forest_clusters.py) scaled to the
    // square's tree count. A cluster stands in for placements whose far asset
    // path contains its match text; a square is clustered when one match
    // covers most of its trees, and the others stay individual impostors.
    // Several clusters with the same match alternate. 0 distance disables.
    struct FarCluster
    {
        std::string asset;
        std::string match;
        float trees = 12.f; // trees baked into the cluster
    };
    std::vector<FarCluster> farClusters;
    float farClusterDistance = 0.f;
    // Far canopy (fair visibility, 2026-11-24): far blocks draw every real
    // tree as its impostor out to this distance from the camera; past it the
    // terrain shows the canopy the macro color map splats from the same
    // placements (LandscapeDriver), fading in over its last 10 %. Nothing
    // stands in for trees that are not there. 0: trees to the far-object
    // radius and the canopy everywhere (as before).
    float farCanopyDistance = 0.f;
    float farClusterSize = 40.f;
    int farClusterMinimumTrees = 5;
    // Negative values inherit the legacy shared threshold.
    float farTileErrorPixelsLod2 = -1.f;
    float farTileErrorPixelsLod4 = -1.f;
    float farTileErrorPixelsLod8 = -1.f;
    float farTileErrorPixelsLod16 = -1.f;
    // Zero follows the camera clip distance.
    float maxFarViewDistanceMeters = 0.f;
    float farTileLodHysteresis = .2f;
    bool farVegetationImpostors = true;
    float farVegetationClusterSpanMeters = 24.f;
    int farVegetationImpostorBudgetPerTile = 96;
    bool batchFarCells = true;
    // Optional fast directional diffuse shading for static play-mode terrain.
    bool fastRuntimeLighting = false;
    // Sun shadows from distant ridges, beyond the realtime cascades: a soft
    // visibility map over the far height grid, ray-marched toward the sun on
    // a worker thread whenever the sun moves. Softness is the penumbra width
    // in degrees of sun elevation.
    bool longRangeShadows = true;
    float longRangeShadowSoftness = 1.5f;
    struct SunShadowMap
    {
        const Engine::Graphics::IGraphicsTexture* texture = nullptr;
        // World X/Z of the map's corner, and 1 / extent along X and Z.
        glm::vec4 mapping{ 0.f };
    };
    // The latest finished map for roughly this sun direction (it may lag a
    // moving sun by one build); empty until the first build completes.
    SunShadowMap GetSunShadowMap(IGraphicsProvider* provider,
        const glm::vec3& directionToSun);
    // Compare stochastic tile variation with the low-cost world-space UV warp.
    bool stochasticTerrainVariation = false;
    // Measured lowest-mip layer colors can replace distant detail in the
    // resident terrain window. Keep opt-in until its GPU cost is lower.
    bool terrainMacroColorEnabled = false;
    // Macro color map: one albedo per sample over the whole map, composed by
    // a scene script (SetMacroColorSource) from paint, erosion and forest
    // cover. Distant tiles take their color from it, and loaded cells blend
    // to it between macroColorNear and macroColorFar (metres from the
    // camera), so both sides of the loaded-cell border show the same color.
    bool macroColorMap = true;
    float macroColorNear = 256.f;
    float macroColorFar = 640.f;
    // Distant forest (macro map alpha) shades as a bumpy canopy: crown- and
    // clump-scale relief in its normals plus crown occlusion, so wooded hills
    // read as forest beyond the far tree impostors. 0 disables.
    float canopyRelief = .6f;
    struct MacroColorImage
    {
        int width = 0, height = 0;
        // Manager-local X/Z of sample (0, 0); samples are spacing metres apart.
        float originX = 0.f, originZ = 0.f, spacing = 16.f;
        // sRGB RGBA8 (alpha: forest cover), full mip chain, mip 0 first.
        std::vector<uint8_t> mips;
        uint32_t mipLevels = 0;
        // Optional world-space surface normals at the same samples (linear
        // RGBA8, xyz * 0.5 + 0.5), same mip chain. Distant tiles are lit per
        // pixel with them, so their 128 m triangles shade like the 16 m terrain.
        std::vector<uint8_t> normalMips;
    };
    // Encodes linear RGB + alpha samples (rows along +Z), and optional unit
    // normals, into a MacroColorImage.
    static MacroColorImage EncodeMacroColorImage(int width, int height,
        float originX, float originZ, float spacing, const std::vector<glm::vec4>& linear,
        const std::vector<glm::vec3>* normals = nullptr);
    struct MacroColorSource
    {
        // Cheap fingerprint of the inputs, polled on the scene thread.
        std::function<uint64_t()> revision;
        // Snapshots the inputs (scene thread) and returns the build, which
        // runs on a worker.
        std::function<std::function<MacroColorImage()>()> makeBuild;
    };
    // Registers (or with an empty source, removes) the composer. Rebuilds
    // whenever its revision changes, also in the editor.
    void SetMacroColorSource(MacroColorSource source);
    struct MacroColorBinding
    {
        const Engine::Graphics::IGraphicsTexture* texture = nullptr;
        // Surface normals over the same mapping (texture slot 2), or null.
        const Engine::Graphics::IGraphicsTexture* normalTexture = nullptr;
        // World X/Z of the texture corner, 1 / extent X, 1 / extent Z.
        glm::vec4 mapping{ 0.f };
    };
    // The current map (texture slot 5), polling the source for changes.
    MacroColorBinding GetMacroColorMap(IGraphicsProvider* provider);
    // Paint samples per cell edge for authored cells (0 = vertex paint).
    // Cells adopt it when a paint stroke first touches them; procedural cells
    // keep vertex paint because their edit archives store vertex weights.
    int paintResolution = 0;
    // Parallax relief within 15 m of the camera (see Terrain::reliefMapping).
    bool reliefMapping = true;
    bool reliefShadows = false; // see Terrain::reliefShadows
    // Show a world-space metric ruler and cell boundaries on every terrain mesh.
    bool measurementGrid = false;
    // Adds static mesh colliders to streamed terrain cells for physics actors.
    bool generateColliders = false;
    // Keep terrain collision active only near the viewer.
    bool streamCollidersByDistance = true;
    // Manage collision for authored Terrain descendants and any nearby
    // prefab cells supplied by the optional streamed grid below.
    bool manageAuthoredCellColliders = false;
    // A square of prefab cells around the authored center, created only near the viewer.
    std::string streamedPrefabPath;
    bool reuseStreamedPrefabCells = false;
    std::string streamedPrefabCellName = "Terrain Cell";
    int streamedPrefabHalfExtent = 0;
    int streamedPrefabRadius = 1;
    size_t GetStreamedPrefabCellCount() const
    { const size_t side = static_cast<size_t>(streamedPrefabHalfExtent) * 2 + 1;
      return streamedPrefabPath.empty() ? 0 : side * side - 1; }
    size_t GetLoadedPrefabCellCount() const { return m_streamedPrefabCells.size(); }
    size_t GetPooledPrefabCellCount() const { return m_streamedPrefabPool.size(); }
    // Activate the current cell and at most one neighbor per axis near an edge.
    float colliderActivationDistance = 1.f;
    void UpdateColliderStreamingAtPosition(const glm::dvec3& worldPosition);
    size_t GetActiveColliderCellCount() const { return m_activeColliderCells; }
    size_t GetManagedColliderCellCount() const
    { return manageAuthoredCellColliders ? m_authoredCollisionCells.size() : m_loaded.size(); }
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
    // Main-thread duration of the most recent near-terrain streaming update.
    // Commit subphases are included in commitMs, not added to totalMs again.
    struct StreamingTiming
    {
        double totalMs = 0.0;
        double housekeepingMs = 0.0;
        double queueMs = 0.0;
        double unloadMs = 0.0;
        double candidatesMs = 0.0;
        double loadOrScheduleMs = 0.0;
        double lodMs = 0.0;
        double commitMs = 0.0;
        double commitSceneMs = 0.0;
        // Parts of commitSceneMs: terrain object, prepared layers, restore.
        double commitTerrainMs = 0.0, commitLayersMs = 0.0, commitRestoreMs = 0.0;
        double commitStitchMs = 0.0;
        double commitLodMs = 0.0;
        double commitGraphicsMs = 0.0;
        double commitPhysicsMs = 0.0;
        double colliderActivationMs = 0.0;
        double batchRefreshMs = 0.0;
        double memoryTrimMs = 0.0;
        double farObjectsMs = 0.0;
        int scheduledCells = 0;
        int committedCells = 0;
        int changedLods = 0;
    };
    const StreamingTiming& GetLastStreamingTiming() const
    { return m_lastStreamingTiming; }
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
    // Far object streaming progress (TerrainManagerFarObjects.cpp). Settled
    // means the window around the current center is fully computed, every
    // block is built and its instances are prepared, or far objects are off.
    struct FarObjectStatus
    {
        size_t computedCells = 0, queuedCells = 0, pendingCells = 0;
        size_t blocks = 0, preparingBlocks = 0;
        bool settled = true;
    };
    FarObjectStatus GetFarObjectStatus() const;
    size_t GetCacheHitCount() const { return m_cacheHits; }
    Terrain* GetLoadedCell(int x, int y) const;
    const std::vector<std::string>& GetObjectLayerNames() const
    { return m_objectLayerNames; }
    bool AddObjectLayer(const std::string& name);
    bool RemoveObjectLayer(const std::string& name);
    bool AddCell(int x, int y);
    bool AddProceduralCell(int x, int y);
    void SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder);
    // The surface a generated object layer may read.
    struct ObjectLayerContext
    {
        Cell cell;
        int cellsX = 0, cellsY = 0;
        float cellWidth = 1.f, cellDepth = 1.f;
        const std::vector<float>* heights = nullptr;
        const std::vector<float>* weights = nullptr; // per vertex and terrain layer
        size_t terrainLayerCount = 0;
        // True when the layer is generated only to collect far-object
        // placements (impostors beyond the loaded cells). Generators can skip
        // layers that never draw that far.
        bool farObjects = false;
    };
    // Fills a named object layer that has no saved placements, such as the
    // forests of a procedural map. Runs on streaming workers (and on the main
    // thread for synchronous loads), so it may only touch the layer and the
    // context. Generated layers are saved with the cell like painted ones.
    using ObjectLayerGenerator = std::function<void(const ObjectLayerContext&,
        const std::string& layerName, TerrainObjectLayer&)>;
    void SetObjectLayerGenerator(ObjectLayerGenerator generator);
    // Main thread, whenever a cell's named layer is restored: the place for
    // a scene script to attach or configure components on the layer object.
    using ObjectLayerSetup = std::function<void(Cell, TerrainObjectLayer&)>;
    void SetObjectLayerSetup(ObjectLayerSetup setup) { m_objectLayerSetup = std::move(setup); }
    void SetStaticCache(std::string directory, uint64_t signature);
    bool ReadBakedCellHeights(Cell cell, std::vector<float>& heights,
        std::vector<float>* weights = nullptr,
        uint32_t* weightLayerCount = nullptr) const;
    std::string GetCacheFilePath(int x, int y) const;
    // Bake a deterministic cell without retaining it in the scene or GPU memory.
    bool BakeProceduralCell(int x, int y, bool force = false);
    bool BakeRuntimeTerrain(bool reuseExistingCells = false);
    bool HasCompleteRuntimeBake() const;
    // Paths along a contour of smooth value noise (two octaves), shaded per
    // pixel on every terrain level (Object.hlsl TerrainPathWeight), so a
    // track narrower than the vertex spacing stays sharp near and a faint
    // continuous line far, instead of paint at the vertices that breaks into
    // spots. Set by the scene script, which also keeps the track out of its
    // paint and placements with the same noise (hash and lattice as
    // Object.hlsl TerrainPathHash). Not serialized.
    struct ContourPaths
    {
        bool enabled = false;
        float periodA = 520.f, weightA = .7f, periodB = 170.f, weightB = .3f;
        uint32_t seedA = 71u, seedB = 73u;
        float contour = .5f;
        // Half-widths from the contour, metres: full path inside core, none
        // past edge. Gradient floor (per metre) keeps flat noise from
        // spreading a track into a clearing.
        float core = 1.6f, edge = 4.2f, gradientFloor = .0006f;
        int layer = 1;                // paint layer the track uses
        uint32_t crossableLayers = 0; // layers it may run over (bit mask)
        float strength = .9f;
        // Height band (world Y): rises over riseStart..riseEnd, falls over
        // fallStart..fallEnd.
        float riseStart = -1.e9f, riseEnd = -1.e9f, fallStart = 1.e9f, fallEnd = 1.e9f;
        glm::vec3 farColor{.4f};      // far tiles (no paint there)
    };
    ContourPaths contourPaths;
    // The path's share (0..1, times strength) at a world position and height,
    // as Object.hlsl shades it at full resolution (no surface rules, no pixel
    // footprint). Ground cover keeps off the track with it, now that the paint
    // no longer carries the path. 0 when contourPaths is off.
    float ContourPathShare(float worldX, float worldZ, float height) const;
    struct FarTerrainPalette
    {
        std::array<glm::vec3, 6> farColors{};
        std::array<glm::vec3, 6> nearColors{};
        std::array<int, TerrainHeightArchive::LayerCount> archiveToNear{};
        std::array<float, 5>
            heightBoundaries{-36.f, -24.f, -12.f, 16.f, 48.f};
        float heightBlendHalfWidth = 8.f;
        // Optional color per far-grid vertex (cell corner), row-major from
        // cell (-halfExtent, -halfExtent), (2 * halfExtent + 2)^2 entries.
        // When present it replaces the height bands, so far tiles can show
        // what the near paint shows (forests, rock, lake beds).
        std::vector<glm::vec3> gridColors;
        int gridHalfExtent = 0;
        // Bilinear grid color at fractional cell coordinates.
        bool SampleGridColor(float cellX, float cellY, glm::vec3& color) const
        {
            const int side = gridHalfExtent * 2 + 2;
            if (gridHalfExtent <= 0 ||
                gridColors.size() != static_cast<size_t>(side) * side) return false;
            const float gx = std::clamp(cellX + gridHalfExtent, 0.f, float(side - 1));
            const float gy = std::clamp(cellY + gridHalfExtent, 0.f, float(side - 1));
            const int x0 = std::min(static_cast<int>(gx), side - 2);
            const int y0 = std::min(static_cast<int>(gy), side - 2);
            const float tx = gx - x0, ty = gy - y0;
            const auto at = [&](int x, int y) { return gridColors[static_cast<size_t>(y) * side + x]; };
            color = glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), tx),
                glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
            return true;
        }
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
    bool AreDesiredFarTilesBuilt() const;
    size_t GetPreparingFarTileCount() const;
    size_t GetQueuedFarTileCount() const;
    size_t GetPreparedFarTileCount() const;
    size_t GetVisibleFarTileVertexCount() const;
    struct FarLevelUsage
    {
        int stride = 1;
        size_t residentTiles = 0, visibleTiles = 0;
        uint64_t cpuBytes = 0, gpuBytes = 0;
    };
    std::vector<FarLevelUsage> GetFarLevelUsage() const;
    size_t GetFarTileNodeCount() const { return m_farTiles.size(); }
    uint64_t GetFarTileUploadCount() const { return m_farTileUploadCount; }
    uint64_t GetFarTileCacheHits() const { return m_farTileCacheHits; }
    uint64_t GetFarTileUploadedBytes() const { return m_farTileUploadedBytes; }
    double GetFarTileRebuildMilliseconds() const
    { return m_farTileRebuildMilliseconds; }
    struct FarUpdateTiming
    {
        double totalMs = 0.0;
        double selectionMs = 0.0;
        double preparationMs = 0.0;
        double uploadMs = 0.0;
        double objectSetupMs = 0.0;
        double meshInstallMs = 0.0;
        double graphicsInitMs = 0.0;
        double meshValidationMs = 0.0;
        double meshBoundsMs = 0.0;
        double meshBufferMs = 0.0;
        double visibilityMs = 0.0;
        double workerBuildMs = 0.0;
        size_t queuedTiles = 0;
        size_t uploadedTiles = 0;
        size_t appearedTiles = 0;
        double maxAppearanceLatencyMs = 0.0;
    };
    const FarUpdateTiming& GetLastFarUpdateTiming() const
    { return m_lastFarUpdateTiming; }
    size_t CountVisibleCoverageGaps(int centerX, int centerY,
        int radius) const;
    size_t GetStoredTerrainCount() const { return m_cells.size(); }
    std::vector<Cell> GetEditedProceduralCells() const
    { return {m_editedProcedural.begin(), m_editedProcedural.end()}; }
    bool RemoveCell(int x, int y);
    bool ApplyBrush(double x, double z, float radius, float strength,
        Terrain::Brush brush, int layer = 0, bool realignObjects = false,
        const Terrain::BrushOptions& options = {});
    // Region editing across cells (TerrainRegionEdit.h), in the manager's
    // local X/Z. Cells under the mask (plus the operation's margin) load if
    // needed and form one height window, so erosion and smoothing flow over
    // cell borders and shared samples stay identical.
    bool EditRegion(const TerrainRegionMask& mask, const TerrainRegionOperation& operation,
        bool realignObjects = false);
    bool PaintRegion(const TerrainRegionMask& mask, float strength, int layer, bool erase);
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
    void PrepareGraphicsResources(IGraphicsProvider* provider) override;
    bool DrawProperties(Engine::Core::IInspectorUi& ui) override;
private:
    std::map<Cell, JsonValue> m_cells;
    std::map<Cell, JsonValue> m_objectLayers;
    std::vector<std::string> m_objectLayerNames;
    std::string m_newObjectLayerName = "Objects";
    void RestoreObjectLayers(Cell cell, Engine::Core::Object& cellObject);
    void CaptureObjectLayers(Cell cell, const Engine::Core::Object& cellObject);
    std::set<Cell> m_proceduralCells;
    std::set<Cell> m_editedProcedural;
    mutable std::set<Cell> m_dirtyProceduralEdits;
    std::function<void(Cell, Terrain&)> m_proceduralBuilder;
    // Volume edits read so far, per cell (GetVolumeEdits).
    mutable std::map<Cell, std::vector<TerrainVolumeEdit>> m_volumeEdits;
    // A cell's heights for a neighbouring volume: loaded, else baked or the
    // height archive (as far seams read them). False when unknown.
    bool NeighbourHeights(Cell cell, std::vector<float>& heights) const;
    // Hands a volume cell its neighbours' heights and edits.
    void FeedVolumeNeighbours(Cell cell, VolumeTerrain& volume) const;
    ObjectLayerGenerator m_objectLayerGenerator;
    ObjectLayerSetup m_objectLayerSetup;

    // Far objects (TerrainManagerFarObjects.cpp).
    struct FarObjectCell
    {
        // Cell-local placements; asset is the far (last LOD) representation.
        std::vector<TerrainObjectLayer::Placement> placements;
        // Far assets that stop drawing at a distance (cliff pieces: their
        // layer's reach), by asset. Others draw as far as the far radius.
        std::map<std::string, float> cullDistances;
    };
    struct FarObjectBlock
    {
        Engine::Core::Object* object = nullptr;
        TerrainObjectLayer* layer = nullptr;
        uint64_t membership = 0;
        // Instances finished preparing since the last placement rebuild.
        // Latched: periodic source re-checks report "preparing" briefly.
        bool prepared = false;
    };
    struct FarObjectJob
    {
        std::vector<Cell> cells;
        std::future<std::vector<std::pair<Cell, FarObjectCell>>> result;
    };
    void UpdateFarObjects(int centerX, int centerY);
    void ClearFarObjects();
    struct NearObjectBlock
    {
        Engine::Core::Object* object = nullptr;
        TerrainObjectLayer* layer = nullptr;
        uint64_t membership = 0;
        std::vector<Cell> members;
        // Instances finished preparing since the last placement rebuild (latched).
        bool prepared = false;
    };
    // Keyed by block (cell coordinates divided by the block size) and layer name.
    std::map<std::pair<Cell, std::string>, NearObjectBlock> m_nearObjectBlocks;
    void UpdateNearObjects();
    void ClearNearObjects();
    // True when every layer of the cell that a near block draws is drawn by a
    // block that has finished building with that cell in it.
    bool NearObjectsReady(Cell cell, const Terrain& terrain) const;
    void FenceFarObjectJobs();
    std::map<Cell, FarObjectCell> m_farObjectCells;
    // Loaded cells whose object layers finished preparing once, keyed to the
    // terrain that did. Their periodic source re-checks briefly report
    // "preparing" again; latching keeps far blocks from rebuilding each time.
    std::map<Cell, const Terrain*> m_farNearCovered;
    std::set<Cell> m_farObjectPending;
    std::vector<FarObjectJob> m_farObjectJobs;
    std::map<Cell, FarObjectBlock> m_farObjectBlocks;
    uint64_t m_farObjectSignature = 0;
    // Incremental bookkeeping, so a frame touches only what changed instead
    // of the whole (2 * farObjectRadius + 1)^2 window (16k cells at 64).
    // The queue holds cells still to compute, nearest block first, and is
    // rebuilt only when the center cell, radius or cell set changes.
    Cell m_farScanCenter{ INT_MIN, INT_MIN };
    int m_farScanRadius = -1;
    size_t m_farScanCellCount = 0;
    std::vector<Cell> m_farObjectQueue;
    size_t m_farObjectQueueNext = 0;
    // Blocks to re-evaluate. Urgent ones (near coverage or radius changed)
    // rebuild at once; the rest wait until none of their cells is still
    // being computed, so a block re-prepares once instead of per batch.
    std::set<Cell> m_farDirtyBlocks;
    std::set<Cell> m_farUrgentBlocks;
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
        bool containsEditedPatch = false;
        float maxHeightError = 0.f;
        float minHeight = 0.f, maxHeight = 0.f;
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        int builtCenterX = 0, builtCenterY = 0, builtNearRadius = 0;
        std::vector<uint8_t> builtVisibility;
        bool built = false, hasGeometry = false;
        bool preparing = false;
        std::optional<std::chrono::steady_clock::time_point> requestedAt;
    };
    struct SunShadowBuild
    {
        glm::vec3 direction{ 0.f };
        float softness = 0.f;
        uint32_t side = 0;
        std::vector<uint8_t> pixels;
        std::shared_ptr<const std::vector<float>> heights;
    };
    std::mutex m_sunShadowMutex;
    std::future<SunShadowBuild> m_sunShadowJob;
    // A finished build waiting for room in the texture upload queue.
    SunShadowBuild m_sunShadowPending;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_sunShadowTexture;
    // Macro color map (TerrainManagerMacroColor.cpp).
    MacroColorSource m_macroSource;
    std::future<MacroColorImage> m_macroJob;
    MacroColorImage m_macroImage; // geometry of the uploaded texture
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_macroTexture;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_macroNormalTexture;
    IGraphicsProvider* m_macroProvider = nullptr;
    uint64_t m_macroRequested = 0;
    std::chrono::steady_clock::time_point m_macroChecked{};
    IGraphicsProvider* m_sunShadowProvider = nullptr;
    // Heights of the uploaded texture, and inputs of the newest request.
    std::shared_ptr<const std::vector<float>> m_sunShadowHeights;
    SunShadowBuild m_sunShadowRequest;
    void ResetSunShadow();
    int m_farHalfExtentCells = 0;
    std::vector<float> m_farHeights;
    std::shared_ptr<const std::vector<float>> m_farHeightsSnapshot;
    std::vector<TerrainHeightArchive::FarPatch> m_farPatches;
    std::set<Cell> m_farPatchCells;
    std::vector<FarTile> m_farTiles;
    void RefreshFarTerrainEditedCells(const std::set<Cell>& changedCells);
    // Loads the existing cells overlapping a local X/Z box (for editing).
    std::set<Cell> LoadCellsForEdit(double minX, double minZ, double maxX, double maxZ);
    // Records edited cells for saving (procedural edits or authored cells)
    // and refreshes the far terrain over them.
    void RecordEditedCells(const std::set<Cell>& changedCells);
    std::string m_farHeightArchivePath;
    FarTerrainPalette m_farPalette;
    uint64_t m_farSourceSignature = 0;
    uint64_t m_farTileUploadCount = 0;
    uint64_t m_farTileCacheHits = 0;
    uint64_t m_farTileUploadedBytes = 0;
    double m_farTileRebuildMilliseconds = 0.0;
    FarUpdateTiming m_lastFarUpdateTiming;
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
        std::optional<Mesh::IndexedGeometryBounds> bounds;
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
    StreamingTiming m_lastStreamingTiming;
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
        Engine::Scene::Scene* scene = nullptr;
        uint64_t sceneGeneration = 0;
        int cellsX = 0, cellsY = 0, lodStride = 1;
        float cellWidth = 1.f, cellDepth = 1.f;
        bool hasSaved = false, edited = false;
        bool requireBake = false;
        JsonValue saved;
        std::string cachePath, editPath;
        std::string bakePath;
        uint64_t bakeOffset = 0, bakeSize = 0;
        std::function<void(Cell, Terrain&)> builder;
        bool compactMesh = false;
        ObjectLayerGenerator layerGenerator;
        std::optional<Terrain::Prepared> prepared;
        bool preparedCacheHit = false;
        std::vector<std::pair<std::string, JsonValue>> objectLayers;
        std::vector<std::unique_ptr<TerrainObjectLayer>> preparedLayers;
    };
    struct PreparedCell
    {
        Cell cell;
        uint64_t revision = 0;
        bool cacheHit = false;
        Terrain::Prepared terrain;
        uint64_t sceneGeneration = 0;
        std::vector<std::unique_ptr<TerrainObjectLayer>> objectLayers;
    };
    std::thread m_prepareWorker;
    mutable std::mutex m_prepareMutex;
    std::condition_variable m_prepareWake;
    std::deque<PrepareTask> m_prepareQueue;
    std::deque<PreparedCell> m_preparedCells;
    std::map<Cell, uint64_t> m_preparingCells;
    std::atomic<uint64_t> m_prepareRevision{1};
    std::atomic<bool> m_stopPrepareWorker{false};
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
    int DesiredPrepareLodStride(Cell cell, int centerX, int centerY) const;
    void StreamCells(int centerX, int centerY, int initialRadius = -1);
    void UnloadCell(Cell cell);
    void Stitch(Cell cell);
    void RefreshRenderBatches(int uploadBudget, int centerX, int centerY,
        int radius);
    void RefreshColliderActivation(int centerX, int centerY);
    void RefreshAuthoredCollisionCells();
    std::map<Cell, Terrain*> m_authoredCollisionCells;
    std::map<Cell, Engine::Core::Object*> m_streamedPrefabCells;
    std::vector<Engine::Core::Object*> m_streamedPrefabPool;
    bool m_startingPrefabCells = false;
    void StreamPrefabCells(int centerX, int centerY, bool unlimited = false);
    uint64_t m_authoredCollisionRevision = 0;
    std::optional<glm::dvec3> m_collisionViewerLocal;
    size_t m_activeColliderCells = 0;
    void TrimMemoryBudget(int centerX, int centerY);
    bool ReserveCacheWrite(const std::string& path, const Terrain& terrain);
    bool SaveEdit(Cell cell, const Terrain& terrain) const;
    bool LoadEdit(Cell cell, Terrain& terrain) const;
    void ConfigureCellPhysics(Engine::Core::Object& object) const;
};
}
