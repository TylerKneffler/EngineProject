#pragma once
#include "Core/Component.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainFarCellDetail.h"
#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include "Core/Components/Terrain/TerrainTrails.h"
#include "Core/Components/Terrain/TerrainFoliage.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include <climits>
#include <map>
#include <array>
#include <limits>
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
    // Loaded cells are drawn and collided as density volumes built from them
    // (VolumeTerrain fromTerrain: their heights and volume edits), shaded
    // with their paint and `volumeSteepLayer` on steep faces. The cell's
    // Terrain holds the seed: heights, paint and placement (there is no
    // height-grid mesh; todo.md "Terrain: volume-first conversion", step 7).
    int volumeSteepLayer = 4;
    // Metres between volume samples. The heights are a 4 m grid, so 2 m
    // keeps their detail and costs an eighth of 1 m (and ~a quarter of
    // the triangles).
    float volumeSampleSpacing = 2.f;
    // Where loaded cells' volumes drop a level (every 2nd, 4th, 8th sample),
    // in metres from the camera to each chunk (VolumeTerrain lod1-3Distance).
    float volumeLod1Distance = 64.f;
    float volumeLod2Distance = 128.f;
    float volumeLod3Distance = 256.f;
    // Pushes the level distances above to every loaded cell's volume.
    void ApplyVolumeLevelDistances();
    // Volume edits (caves, arches; TerrainVolumeEdit), kept per cell in
    // <cellDataDirectory>/<x>_<z>.volumeedit in the cell's space, read when
    // the cell's volume is built (with its neighbours', which may reach
    // across). Loaded volume cells mesh them; far tiles mesh the far cells
    // they reach from the field (FarVolumeCells); ground queries read the
    // field (GetTerrainField).
    const std::vector<TerrainVolumeEdit>& GetVolumeEdits(Cell cell) const;
    // Cells that own a .volumeedit file (cellDataDirectory, scanned once;
    // AddVolumeEdit adds to it).
    const std::set<Cell>& VolumeEditOwners() const;
    // A far cell volume edits reach, with those edits in its own space
    // (own first, then the neighbours', FeedVolumeNeighbours order).
    struct FarVolumeCell
    {
        Cell cell;
        std::vector<TerrainVolumeEdit> edits;
    };
    std::vector<FarVolumeCell> FarVolumeCells(int startX, int startY, int endX, int endY) const;
    // Adds an edit given in the grid's space to the cell holding its centre,
    // saves that cell's file, and rebuilds the loaded volume cells it
    // reaches (their current chunks draw until the new ones are ready).
    // False when the file cannot be written; an edit a cell or more across
    // is refused. followObjects: placements of the loaded cells under the
    // edit move onto the new surface (FollowVolumeEdit).
    bool AddVolumeEdit(const TerrainVolumeEdit& edit, bool followObjects = false);
    // Placements of loaded cells within [low, high] (grid space) after the
    // ground there changed: buried ones rise to the top surface, ones left in
    // the air drop to the first ground below; ones then standing on ground
    // steeper than about 60 degrees (a carved wall) or on none are removed.
    // Returns how many moved or went.
    size_t FollowVolumeEdit(const glm::vec3& low, const glm::vec3& high);
    // Replaces a cell's whole edit list (saving or deleting its file), and
    // rebuilds what it reaches, as AddVolumeEdit does. The scene's copy of
    // the edits (Serialize "volumeEdits") comes back through this on load
    // and on undo.
    bool SetVolumeEdits(Cell cell, std::vector<TerrainVolumeEdit> edits);
    std::string GetVolumeEditFilePath(Cell cell) const;
    // The terrain as one field (TerrainField.h; todo.md "Terrain: volume-first
    // conversion"): ground queries (SurfaceAt, Raycast, Density) over the
    // seed heights (height archive plus height edits, or the procedural
    // builder) and every cell's volume edits. Safe to keep and query on any
    // thread; a new one is made when the grid, archive or data directory
    // changes. Edits through RecordEditedCells and AddVolumeEdit update it.
    std::shared_ptr<const TerrainField> GetTerrainField();
    // Changes whenever the field (or any of its cells) changes.
    uint64_t GetTerrainFieldRevision() const { return m_terrainFieldRevision; }
    // The field when volume edits reach the cell a loaded Terrain draws,
    // with that cell's origin in the field's space; null where the cell is
    // exactly its heights (no edits), so callers keep the height grid.
    std::shared_ptr<const TerrainField> EditedFieldFor(const Terrain& terrain,
        float& originX, float& originZ);
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
    // placements (canopyCrownTable), fading in over its last 10 %. Nothing
    // stands in for trees that are not there. 0: trees to the far-object
    // radius and the canopy everywhere (as before).
    float farCanopyDistance = 0.f;
    float farClusterSize = 40.f;
    int farClusterMinimumTrees = 5;
    // Negative values inherit the legacy shared threshold.
    // Distant terrain (TerrainManagerFarVolume.cpp): volume nodes 1 to 64
    // cells across split while the viewer is within farVolumeDetail of their
    // own size (larger: finer, more triangles). The node set is chosen again
    // after the viewer moves farVolumeStepMetres.
    float farVolumeDetail = 2.f;
    float farVolumeStepMetres = 32.f;
    // Main-thread time per update for installing built distant nodes (at
    // least one a frame); more waits for the next frame.
    float farVolumeInstallMilliseconds = 1.5f;
    // Terrain virtual texture (DX12; Rendering/SceneRenderer/SceneTerrainVirtualTexture.h):
    // the blended layer material cached on a clipmap around the camera, so
    // flat-enough ground reads it instead of blending its layers per pixel.
    // Level 0 is virtualTextureResolution texels of virtualTextureTexelMillimetres;
    // each further level doubles the texel (6 levels of 4 mm x 2048 reach ~260 m,
    // ~300 MB). Windows follow the camera and are trusted within 3/8 of their
    // extent, so a level needs ~2500 texels across for the level matching a
    // pixel's footprint to be in range: at 1024 the ground blurs from ~1.5 m.
    bool virtualTexture = false;
    int virtualTextureLevels = 6;
    int virtualTextureResolution = 2048;
    float virtualTextureTexelMillimetres = 4.f;
    // Zero follows the camera clip distance.
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
        // The samples it was encoded from (normals may be empty), kept so
        // edited cells can be recomposed without a whole build.
        std::shared_ptr<const std::vector<glm::vec4>> linear;
        std::shared_ptr<const std::vector<glm::vec3>> normals;
    };
    // Encodes linear RGB + alpha samples (rows along +Z), and optional unit
    // normals, into a MacroColorImage.
    static MacroColorImage EncodeMacroColorImage(int width, int height,
        float originX, float originZ, float spacing, const std::vector<glm::vec4>& linear,
        const std::vector<glm::vec3>* normals = nullptr);
    // New samples for a rectangle of a map (a cell's, after an edit): x0, y0
    // the first sample, colors and normals (optional) width * height each,
    // rows along +Z.
    struct MacroColorPatch
    {
        int x0 = 0, y0 = 0, width = 0, height = 0;
        std::vector<glm::vec4> colors;
        std::vector<glm::vec3> normals;
    };
    // The image with the patches written over its samples, encoded again.
    // Patches outside the map are clipped. Empty when `image` keeps no samples.
    static MacroColorImage PatchMacroColorImage(const MacroColorImage& image,
        const std::vector<MacroColorPatch>& patches);
    struct MacroColorSource
    {
        // Cheap fingerprint of the inputs, polled on the scene thread.
        std::function<uint64_t()> revision;
        // Snapshots the inputs (scene thread) and returns the build, which
        // runs on a worker.
        std::function<std::function<MacroColorImage()>()> makeBuild;
        // Optional: recomposes the samples of edited cells from the terrain
        // as it is now (sculpts, volume edits, paint, erosion). Called on the
        // scene thread with the cells and the current map; returns the job
        // (run on a worker) that gives their patches. Without it, edits show
        // at the next whole build.
        std::function<std::function<std::vector<MacroColorPatch>()>(
            const std::vector<Cell>& cells, const MacroColorImage& map)> makeCellUpdate;
    };
    // Cells whose macro colors wait for recomposing (MacroColorSource::
    // makeCellUpdate): every edit marks its cell and the eight around it
    // (slopes and tree crowns reach across), and a whole build marks the
    // edited cells, which it reads from the source as generated.
    std::vector<Cell> GetMacroDirtyCells() const
    { return {m_macroDirtyCells.begin(), m_macroDirtyCells.end()}; }
    void MarkMacroCellsDirty(Cell cell);
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
    // Object layer settings by layer name (view, LOD and shadow distances),
    // saved with the scene. A layer made in a cell (painting, a new name)
    // takes them; setting them applies to every loaded cell's layer of that
    // name. Null when the name has none (the layer keeps its own).
    const TerrainObjectLayer::Settings* GetObjectLayerSettings(const std::string& name) const;
    void SetObjectLayerSettings(const std::string& name, const TerrainObjectLayer::Settings& settings);
    void ApplyObjectLayerSettings(TerrainObjectLayer& layer) const;
    // Object placements of procedural cells (TerrainManager::cellDataDirectory
    // set) live in one file per cell, <dir>/<x>_<z>.objects, written only
    // when a cell's layers differ from what it loaded with (painted,
    // erased, moved by sculpting); untouched cells generate theirs again, so
    // the scene holds none of them. Undo restores earlier revisions of the
    // files written this session (Serialize lists each file's revision).
    // A cell's saved layers: the scene's (authored cells), its .objects file
    // (an edit), else its block of the object archive. False: none (layers
    // then start empty, or a generator fills them).
    bool GetSavedObjectLayers(Cell cell, JsonValue& layers) const;
    // Baked object placements of procedural cells (TerrainObjectArchive, an
    // Assets path): what a map's trees, rocks and bushes start as. A cell's
    // .objects file wins over its block.
    std::string objectArchive;
    std::shared_ptr<const class TerrainObjectArchive> GetObjectArchive() const;
    // A map stored as archives (no script needed): procedural cells read
    // their heights from heightArchive (TerrainHeightArchive, Assets path)
    // and their paint from paintArchive (TerrainPaintArchive: per vertex
    // weights of terrainLayers, plus lushness; cells it lacks are all
    // defaultPaintLayer), with terrainLayers' materials repeating every
    // layerRepeatMetres. Built cells are cached in cacheDirectory, keyed by
    // the archives and layers. A script's SetProceduralBuilder replaces it.
    struct TerrainLayerSource
    {
        std::string material;
        glm::vec3 preview{.5f};   // editor and distant colour before textures load
    };
    std::string heightArchive, paintArchive, cacheDirectory;
    std::vector<TerrainLayerSource> terrainLayers;
    int defaultPaintLayer = 0;
    float layerRepeatMetres = 2.f;
    // The height archive's half extent in cells and signature (0 and 0
    // without one); the paint archive (null without one).
    int ArchiveHalfExtent() const;
    uint64_t ArchiveSignature() const;
    std::shared_ptr<const class TerrainPaintArchive> GetPaintArchive() const;
    // Installs the archive builder and its cache for heightArchive (nothing
    // without one, or when a script set its own builder). Runs on load; call
    // again after changing the archives. True when the archive terrain is set.
    bool ConfigureArchiveTerrain();
    // What the archive map shows beyond its loaded cells (TerrainManager
    // ArchiveMap.cpp), from the same archives: the far terrain (corner
    // heights, colours from the paint) when archiveFarTerrain, and the macro
    // colour map (macroColorMap: paint, lushness, steep faces, tree canopy).
    // Colours are the terrain layers' macro colours times
    // archiveColorBrightness (below 1 makes up for the occlusion and texture
    // contrast near shading adds); lush ground takes the tint near cells
    // show (Object.hlsl), steep faces steepMaterial's colour (none when
    // empty), and trees with
    // a crown in canopyCrownTable (JSON from tools/measure_tree_crowns.py)
    // cover the macro map as canopy.
    bool archiveFarTerrain = true;
    float archiveColorBrightness = .85f;
    std::string steepMaterial, canopyCrownTable;
    // Fingerprint of everything the far terrain and macro map read (0
    // without an archive).
    uint64_t ArchiveMapKey() const;
    std::string GetObjectFilePath(Cell cell) const;
    // Cells that have an .objects file.
    const std::set<Cell>& ObjectFileCells() const;
    // Writes the files of loaded cells whose objects changed (also done by
    // Serialize and when a cell unloads). Returns how many were written.
    size_t SaveObjectFiles() const;
    // A cell's far-object placements are computed again (its saved objects
    // changed).
    void InvalidateFarObjectCell(Cell cell);
    bool RemoveObjectLayer(const std::string& name);
    bool AddCell(int x, int y);
    bool AddProceduralCell(int x, int y);
    // Procedural cells: every cell within halfExtent of the origin (a square
    // of 2 * halfExtent + 1 a side; authored cells stay as they are). The
    // map-size control of the terrain generator. Loaded procedural cells
    // outside it unload.
    void SetProceduralSquare(int halfExtent);
    // The cell data's generation (cell_data.generation in cellDataDirectory,
    // renewed by the landscape generator when it re-seats the files). A scene
    // saved under another generation lists files (object files, volume edits,
    // foliage) that no longer apply: loading it leaves the files as they are.
    std::string CellDataGeneration() const;
    void SetProceduralBuilder(std::function<void(Cell, Terrain&)> builder);
    // The builder set without changing whether it is the archive's.
    void InstallBuilder(std::function<void(Cell, Terrain&)> builder);
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
        // The terrain field, set when volume edits reach this cell (a crater,
        // an arch, an overhang): ground queries then follow the sculpted
        // surface instead of the seed heights. Null elsewhere, where the
        // field is exactly the heights.
        std::shared_ptr<const TerrainField> field;
        // The ground under (x, z) in the cell's own space (may reach past
        // its edges into neighbours when the field is set): the top surface
        // under open sky, as the heights' two triangles where no edit is.
        bool SampleSurface(float x, float z, float& height, glm::vec3* normal = nullptr) const;
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
    // Trails (TerrainTrails.h): polylines that scene objects publish, keyed
    // by the publisher (TerrainTrail scripts: a SplinePath per trail object,
    // drawn at startup and whenever its points move). The network is shaded
    // per pixel on every terrain level (Object.hlsl TerrainTrailSample),
    // carved into procedural cells as they build, and ground cover and
    // placement rules keep off it. Not serialized: the trail objects are.
    // Foliage density (TerrainFoliage.h): where dense ground cover grows,
    // per foliage layer, in 3D. Only painted chunks are stored, one
    // <x>_<z>.foliage file per cell in cellDataDirectory, written when the
    // manager is serialized (scene save, undo snapshots) and listed there by
    // signature so a snapshot brings the painted state back.
    // The cell's painted chunks (never null; empty where none).
    std::shared_ptr<const TerrainFoliageCell> GetFoliage(Cell cell) const;
    // Changes whenever that cell's foliage does.
    uint64_t GetFoliageRevision(Cell cell) const;
    // The cell a loaded cell's terrain is.
    std::optional<Cell> CellOf(const Terrain& terrain) const;
    // A brush dab on a foliage layer, over the loaded cells it reaches. The
    // layer's baseline comes from the component growing it in each cell (a
    // TerrainFoliageSource whose FoliageLayerName matches). True when a
    // density changed.
    bool PaintFoliage(const std::string& layerName, const TerrainFoliageStroke& stroke);
    // Replaces a cell's foliage (tools, restores).
    void SetFoliage(Cell cell, std::shared_ptr<const TerrainFoliageCell> foliage);
    std::string GetFoliageFilePath(Cell cell) const;
    size_t SaveFoliageFiles() const;
    void SetTrail(uint64_t key, TerrainTrail trail);
    void RemoveTrail(uint64_t key);
    // Called every editor frame (UpdateEditorStreaming), where scripts get no
    // Update: a publisher checks its points there and republishes on change.
    void SetTrailWatcher(uint64_t key, std::function<void()> watcher);
    void RemoveTrailWatcher(uint64_t key) { m_trailWatchers.erase(key); }
    // The current network (never null); safe to call and keep on any thread.
    // Loops should take it once rather than call TrailShare per sample.
    std::shared_ptr<const TerrainTrailNetwork> GetTrails() const;
    // The track's share (0..1, times strength) at a position (manager
    // space), as the terrain shades it at full resolution (no paint rules,
    // no pixel footprint). 0 without trails.
    float TrailShare(float x, float z) const { return GetTrails()->Share(x, z); }
    // Metres from the nearest trail's centre line (manager space); infinite
    // away from every trail.
    float TrailDistance(float x, float z) const { return GetTrails()->Distance(x, z); }
    // The network for the renderer: an RGBA32F texture (TerrainTrailNetwork::
    // PackGpu; null without trails) and its grid constants.
    struct TrailGpuData
    {
        const Engine::Graphics::IGraphicsTexture* texture = nullptr;
        glm::vec4 grid{0.f};
        glm::uvec4 keys{0u};
    };
    TrailGpuData GetTrailGpuData(IGraphicsProvider* provider);
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
        // Steep faces shade toward this colour (negative: off), by the far
        // vertex's own normal: the grid's 128 m colours averaged a cliff into
        // its surroundings and the bands know only height, so distant cliffs
        // were pale meadow tan where near volume cells show scanned rock.
        // Same measure and band as VolumeTerrain (1 - normal.y, steepStart
        // .. steepEnd), so near and far turn to rock on the same slopes.
        glm::vec3 steepColor{-1.f};
        float steepStart = .25f, steepEnd = .45f;
        void ApplySteep(float* color, float normalY) const
        {
            if (steepColor.x < 0.f) return;
            float t = std::clamp((1.f - normalY - steepStart) / std::max(steepEnd - steepStart, 1e-3f), 0.f, 1.f);
            t = t * t * (3.f - 2.f * t);
            for (int channel = 0; channel < 3; ++channel)
                color[channel] += (steepColor[channel] - color[channel]) * t;
        }
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
    size_t GetFarTileNodeCount() const { return m_farNodes.size(); }
    // The far grid's height at a cell corner (NaN outside it), and whether
    // far tiles draw a cell from its own full heights (an edited cell).
    float GetFarCornerHeight(int x, int z) const
    {
        const int side = m_farHalfExtentCells * 2 + 2, gx = x + m_farHalfExtentCells,
            gz = z + m_farHalfExtentCells;
        return gx < 0 || gz < 0 || gx >= side || gz >= side ||
            m_farHeights.size() != static_cast<size_t>(side) * side
            ? std::numeric_limits<float>::quiet_NaN() : m_farHeights[static_cast<size_t>(gz) * side + gx];
    }
    bool HasFarPatch(Cell cell) const { return m_farPatchHeights && m_farPatchHeights->count(cell); }
    uint64_t GetFarTileUploadCount() const { return m_farTileUploadCount; }
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
    bool PaintRegion(const TerrainRegionMask& mask, float strength, int layer, bool erase,
        const Engine::Brushes::BrushRules* rules = nullptr);
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
    mutable std::map<Cell, JsonValue> m_objectLayers;
    std::vector<std::string> m_objectLayerNames;
    std::string m_newObjectLayerName = "Objects";
    void RestoreObjectLayers(Cell cell, Engine::Core::Object& cellObject);
    void CaptureObjectLayers(Cell cell, const Engine::Core::Object& cellObject);
    std::map<std::string, TerrainObjectLayer::Settings> m_objectLayerSettings;
    bool UsesObjectFiles(Cell cell) const;
    static JsonValue CollectObjectLayers(const Engine::Core::Object& cellObject);
    // What an edit changes in a cell's layers: names, settings and
    // placements (asset, position, normal, yaw, scale). Not ids or prepared
    // instance data, which the runtime fills in after a cell loads.
    static uint64_t HashCellObjects(const Engine::Core::Object& cellObject);
    // Remembers a freshly loaded cell's layers as its baseline (unchanged
    // cells write no file).
    void SetObjectBaseline(Cell cell, const Engine::Core::Object& cellObject);
    // Writes (or with null removes) a cell's file as a new revision.
    bool WriteObjectFile(Cell cell, const JsonValue* layers) const;
    uint64_t ObjectFileRevision(Cell cell) const;   // 0: no file
    // Restores files to the revisions a scene lists (undo, redo, load).
    void RestoreObjectFiles(const JsonValue& listed);
    mutable std::map<Cell, uint64_t> m_objectBaseline;
    mutable std::shared_ptr<const class TerrainObjectArchive> m_objectArchive;
    mutable std::string m_objectArchiveOpened;
    mutable std::optional<std::set<Cell>> m_objectFileCells;
    mutable std::map<Cell, uint64_t> m_objectFileRevisions;
    // Scene-saved layers of file-backed cells (older scenes): written to
    // files at the next save.
    mutable std::set<Cell> m_objectFilesPending;
    std::set<Cell> m_proceduralCells;
    std::set<Cell> m_editedProcedural;
    mutable std::set<Cell> m_dirtyProceduralEdits;
    std::function<void(Cell, Terrain&)> m_proceduralBuilder;
    // ConfigureArchiveTerrain: whether the builder is the archive's, and the
    // key it was installed for.
    bool m_archiveBuilder = false;
    uint64_t m_archiveKey = 0;
    // The archive map (TerrainManagerArchiveMap.cpp).
    struct ArchiveFarGrid
    {
        bool loaded = false;
        std::vector<float> heights;
        std::vector<glm::vec3> colors;
    };
    std::future<ArchiveFarGrid> m_archiveFarJob;
    uint64_t m_archiveFarJobKey = 0, m_archiveFarKey = 0;
    bool m_archiveFarActive = false, m_archiveMacroInstalled = false;
    mutable std::shared_ptr<const void> m_canopyCrowns;
    mutable std::string m_canopyCrownsLoaded;
    std::shared_ptr<const void> ArchiveCanopyCrowns() const;
    FarTerrainPalette ArchiveFarPalette() const;
    void InstallArchiveMacroColors();
    std::function<std::vector<MacroColorPatch>()> MakeArchiveMacroCellUpdate(
        const std::vector<Cell>& cells, const MacroColorImage& map);
    // Configures the far terrain for the current key: waits for it, or
    // starts / polls a worker. True once it is configured.
    bool LoadArchiveFarTerrain(bool wait);
    // Polls the far terrain and moves it with the viewer (play mode).
    void UpdateArchiveMap(const Engine::Core::Object* viewer);
    // On load: the cells around the viewer, the far terrain and the editor
    // camera, so the map shows at once.
    void PreviewArchiveMap();
    mutable std::shared_ptr<const class TerrainPaintArchive> m_paintArchive;
    mutable std::string m_paintArchiveOpened;
    // Trails (SetTrail): the live network, shaded and read by rules at once,
    // and the one carved into procedural cells, which follows it after a
    // short pause (edits come in bursts while points are dragged). The
    // carved network's signature is part of the cell cache signature.
    struct TrailState
    {
        mutable std::mutex mutex;
        std::shared_ptr<const TerrainTrailNetwork> live, carved;
    };
    std::shared_ptr<TrailState> m_trailState = std::make_shared<TrailState>();
    std::map<uint64_t, TerrainTrail> m_trailSources;
    // Foliage: cells read or painted so far, those painted since the last
    // save, and a revision per cell.
    mutable std::map<Cell, std::shared_ptr<const TerrainFoliageCell>> m_foliage;
    mutable std::set<Cell> m_foliageDirty;
    std::map<Cell, uint64_t> m_foliageRevisions;
    uint64_t m_foliageRevisionCounter = 0;
    void RestoreFoliageFiles(const JsonValue& listed);
    std::map<uint64_t, std::function<void()>> m_trailWatchers;
    bool m_trailCarvePending = false;
    std::chrono::steady_clock::time_point m_trailChangedAt{};
    uint64_t m_baseCacheSignature = 0;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_trailTexture;
    IGraphicsProvider* m_trailTextureProvider = nullptr;
    std::shared_ptr<const TerrainTrailNetwork> m_trailTextureNetwork;
    std::array<glm::vec3, 6> m_trailTexturePalette{};
    TrailGpuData m_trailGpu;
    // Rebuilds the live network from m_trailSources.
    void RebuildTrailNetwork();
    // Applies the live network to carving (see TrailState) and reloads the
    // loaded procedural cells it changes.
    void ApplyTrailCarving();
    // Streaming: applies a pending carve once trail edits have settled.
    void UpdateTrailCarving();
    // Volume edits read so far, per cell (GetVolumeEdits).
    mutable std::map<Cell, std::vector<TerrainVolumeEdit>> m_volumeEdits;
    mutable std::optional<std::set<Cell>> m_volumeEditOwners;
    // Rebuilds the loaded volume cells within reach of [low, high] (grid
    // space) after an owner's edit list changed.
    void RefreshVolumeCells(Cell owner, const glm::vec3& low, const glm::vec3& high);
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
        // The cells it showed at its last walk and their cull distances:
        // a crossing re-checks blocks away from the radius edge from these
        // instead of walking their cells again (UpdateFarObjects).
        std::vector<Cell> members;
        std::map<std::string, float> cullDistances;
        bool membersValid = false;
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
    // Layer materials and tiling for distant nodes (from the first loaded
    // cell); handed to every node that lacks them (TerrainManagerFarVolume).
    std::vector<std::shared_ptr<Material>> m_farLayerMaterials;
    float m_farLayerTiling = 1.f;
    // Blocks a crossing re-checks from their cached members (see above).
    std::set<Cell> m_farRecheckBlocks;
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
    // Distant terrain nodes (TerrainManagerFarVolume.cpp).
    struct FarVolumeKey
    {
        int x = 0, z = 0, size = 1;   // first cell and cells across
        bool operator<(const FarVolumeKey& other) const
        { return x != other.x ? x < other.x : z != other.z ? z < other.z : size < other.size; }
        bool operator==(const FarVolumeKey& other) const
        { return x == other.x && z == other.z && size == other.size; }
        bool operator!=(const FarVolumeKey& other) const { return !(*this == other); }
    };
    struct FarVolumeMesh
    {
        Mesh::TerrainMeshData geometry;
        std::vector<uint32_t> occluderIndices;
        size_t occluderVertexCount = 0;
        double buildMilliseconds = 0.0;
    };
    struct FarVolumeNode
    {
        std::future<std::shared_ptr<FarVolumeMesh>> job;
        Engine::Core::Object* object = nullptr;
        Mesh* mesh = nullptr;
        size_t vertexCount = 0;
        bool built = false, stale = false, hasGeometry = false;
    };
    struct FarVolumeJobInput;
    static std::shared_ptr<FarVolumeMesh> BuildFarVolumeNode(const FarVolumeJobInput& input);
    std::map<FarVolumeKey, FarVolumeNode> m_farNodes;
    std::vector<FarVolumeKey> m_farWanted, m_farShown, m_farBuildOrder;
    std::set<Cell> m_farShownCells;
    glm::dvec3 m_farStep{0.0};
    bool m_farStepValid = false, m_farDirty = false;
    std::shared_ptr<const FarCellDetail::Archive> m_farArchive;
    std::future<std::shared_ptr<const FarCellDetail::Archive>> m_farArchiveJob;
    std::shared_ptr<const std::map<Cell, std::vector<float>>> m_farPatchHeights;
    std::shared_ptr<const FarTerrainPalette> m_farPaletteShared;
    void FenceFarVolumeJobs();
    // Far nodes reaching a cell build again (its heights or edits changed).
    void MarkFarVolumeDirty(Cell cell);
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
    std::set<Cell> m_macroDirtyCells;
    // What the running job is: a whole build, or a patch of these cells.
    bool m_macroJobWhole = false;
    std::vector<Cell> m_macroJobCells;
    // After a patch: far tiles' corner colors (FarTerrainPalette::gridColors)
    // follow the patched cells' corner samples.
    void UpdateFarGridColors(const std::vector<Cell>& cells);
    IGraphicsProvider* m_sunShadowProvider = nullptr;
    // Heights of the uploaded texture, and inputs of the newest request.
    std::shared_ptr<const std::vector<float>> m_sunShadowHeights;
    SunShadowBuild m_sunShadowRequest;
    void ResetSunShadow();
    int m_farHalfExtentCells = 0;
    std::vector<float> m_farHeights;
    std::shared_ptr<const std::vector<float>> m_farHeightsSnapshot;
    void RefreshFarTerrainEditedCells(const std::set<Cell>& changedCells);
    // Loads the existing cells overlapping a local X/Z box (for editing).
    std::set<Cell> LoadCellsForEdit(double minX, double minZ, double maxX, double maxZ);
    // Records edited cells for saving (procedural edits or authored cells)
    // and refreshes the far terrain over them.
    void RecordEditedCells(const std::set<Cell>& changedCells);
    std::string m_farHeightArchivePath;
    // TerrainField source (TerrainManagerField.cpp): shared with the field's
    // seed, which runs on any thread.
    struct FieldSource;
    std::shared_ptr<FieldSource> m_fieldSource;
    std::shared_ptr<TerrainField> m_terrainField;
    uint64_t m_terrainFieldKey = 0;
    uint64_t m_terrainFieldRevision = 1;
    void InvalidateTerrainField(Cell cell, const std::vector<float>* heights = nullptr);
    void SeedAuthoredCells();
    // ENGINE_TERRAIN_FIELD_CHECK=1: once the loaded area is full, compare the
    // field's surface with every loaded cell's heights and print the result.
    void CheckTerrainField();
    bool m_terrainFieldChecked = false;
    FarTerrainPalette m_farPalette;
    uint64_t m_farSourceSignature = 0;
    uint64_t m_farTileUploadCount = 0;
    FarUpdateTiming m_lastFarUpdateTiming;
    bool m_farEnabled = false;
    uint64_t m_uploadBudgetFrame = UINT64_MAX;
    size_t m_nearFrameUploads = 0;
    size_t m_farFrameUploads = 0;
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
        ObjectLayerGenerator layerGenerator;
        std::shared_ptr<const TerrainField> layerField; // when volume edits reach the cell
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
