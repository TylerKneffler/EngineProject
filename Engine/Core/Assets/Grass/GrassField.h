#pragma once

#include "Core/Components/Script.h"
#include "Core/Components/SpatialUpdate.h"
#include "Core/Components/PropertyMacros.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Mesh/MeshInstanceLayerData.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Grass/GrassPerformance.h"
#include <chrono>
#include <optional>
#include <map>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Engine::Components
{
class Material;
class Terrain;
class TerrainObjectLayer;
}

namespace Engine::Scene { class Scene; }

// Deterministic tiles sharing species geometry. Only compact placement records
// are prepared on the CPU; all blade vertex transforms run on the GPU.
class GrassField final : public Engine::Core::Script, public Engine::Components::SpatialUpdate, public Engine::Components::ObjectLodProvider
{
public:
    GrassField();

    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string shortMesh = "Assets/Mesh/Grass/meadow_short.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string tallMesh = "Assets/Mesh/Grass/field_tall.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string shortMeshLod1 = "Assets/Mesh/Grass/meadow_short_lod1.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string shortMeshLod2 = "Assets/Mesh/Grass/meadow_short_lod2.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string tallMeshLod1 = "Assets/Mesh/Grass/field_tall_lod1.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string tallMeshLod2 = "Assets/Mesh/Grass/field_tall_lod2.obj";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Viewer")
    std::string viewerObjectName = "Grass Test Capsule";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Viewer")
    bool useEditorCamera = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Viewer", ClampMin = "2")
    float visibleRadius = 11.f;
    // Multiplier on the grass reach from the owner's project layer.
    float DetailRadiusScale() const;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "1")
    float tileSize = 4.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "0")
    float shortPatchesPerSquareMeter = .5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "0")
    float tallPatchesPerSquareMeter = .16f;
    // Wildflowers and weeds scattered through the grass: semicolon-separated
    // LOD0 meshes. "<name>_lod1.obj"/"<name>_lod2.obj" siblings are used when
    // present (tools/generate_meadow_accents.py writes all three).
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Accents")
    std::string accentMeshes;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Accents", ClampMin = "0")
    float accentPatchesPerSquareMeter = 0.f;
    // Procedural accents grow in drifts: each cell of this size (metres)
    // favours one species and has its own richness.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Accents", ClampMin = "1")
    float accentDriftSize = 7.f;
    // Low ground-cover shrubs (semicolon-separated LOD0 meshes with optional
    // _lod1/_lod2 siblings, as for accents; tools/generate_meadow_grass.py).
    // Procedural shrubs grow in thickets that close over the ground.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Shrubs")
    std::string shrubMeshes;
    // Shrubs per square metre at the heart of a thicket.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Shrubs", ClampMin = "0")
    float shrubPatchesPerSquareMeter = 0.f;
    // Typical distance between thickets (metres): the period of the thicket noise.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Shrubs", ClampMin = "1")
    float shrubThicketSize = 9.f;
    // Roughly the share of the ground that thickets cover (0..1).
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Shrubs", ClampMin = "0", ClampMax = "1")
    float shrubCoverage = .3f;
    // Larger, solid bushes (about 1-2 m; semicolon-separated LOD0 meshes with
    // optional _lod1/_lod2 siblings; tools/generate_meadow_grass.py). They
    // grow in loose clusters with a few scattered singles, and grass thins
    // beneath them.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Bushes")
    std::string bushMeshes;
    // Bushes per square metre at the heart of a cluster.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Bushes", ClampMin = "0")
    float bushesPerSquareMeter = 0.f;
    // Typical distance between bush clusters (metres).
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Bushes", ClampMin = "1")
    float bushClusterSize = 16.f;
    // Roughly the share of the ground inside bush clusters (0..1).
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Bushes", ClampMin = "0", ClampMax = "1")
    float bushCoverage = .25f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles")
    int seed = 18731;
    // Retained for scene compatibility; placement publication is capped at one
    // tile per refresh and shares the scene's preparation/upload budget.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "1", ClampMax = "16")
    int maxTileBuildsPerUpdate = 4;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "0.1", ClampMax = "16")
    float tileWorkBudgetMs = 2.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Lighting")
    bool castShadows = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Lighting")
    std::string shaderPath = "Assets/Shaders/GrassFootBend.hlsl";
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Assets")
    std::string materialPath;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Lighting", ClampMin = "0", ClampMax = "1")
    float windStrength = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0")
    float fadeDuration = .35f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD")
    bool gpuLodSelection = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles")
    bool allowProceduralGeneration = false;
    // Procedural patches grow only where these terrain paint layers (bit per
    // layer index) together hold at least half the weight. 0 grows anywhere.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles")
    int proceduralLayerMask = 0;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles")
    bool retainDormantTiles = false;
    // Legacy tile fade width (metres), used only when distanceFadeFraction is 0.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0")
    float distanceFadeWidth = 2.f;
    // Share of the grass radius over which each plant shrinks into the ground
    // at the range edge (per placement, on the GPU). 0 falls back to fading
    // whole tiles over distanceFadeWidth.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0", ClampMax = "0.9")
    float distanceFadeFraction = .3f;
    // Share of plants still drawn where the range fade begins: from a fifth
    // of the radius the share falls to this, and the survivors widen so the
    // cover looks as dense. Fewer plants at range lets LODs keep more detail
    // and the radius reach further. 1 disables thinning.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0.1", ClampMax = "1")
    float distanceThinning = .45f;
    // Far ring: when > 0 this field only grows beyond this distance (metres),
    // fading in over innerFadeWidth where a detailed field fades out. Used
    // for a cheap distant layer (large tiles, simple tufts) that carries the
    // grass out past the detailed range. Its plants do not thin with
    // distance, and it leaves the ground tint to the detailed field.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0")
    float innerRadius = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0.5")
    float innerFadeWidth = 10.f;
    // How strongly the terrain under and beyond procedural grass takes the
    // grass colour (darker roots between plants, the blade colour past the
    // grass range). 0 leaves the terrain layer untouched.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Ground", ClampMin = "0", ClampMax = "1")
    float groundTintStrength = .5f;
    // Root colour between plants as a share of the mean grass colour.
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Ground", ClampMin = "0", ClampMax = "1")
    float groundRootShade = .55f;

    std::map<std::string, Engine::Components::ObjectLodProfileData>
        GetObjectLodProfiles() const override;
    void Start() override;
    void Update() override;
    bool RequiresUpdate() const override { return false; }
    // Ground cover lives in the project's grass detail budget.
    const char* PreferredDetailLayer() const override { return "Grass"; }
    uint64_t SpatialRegionRevision() const override;
    Engine::Components::SpatialUpdateRegion DescribeSpatialRegion() override;
    bool TickSpatialRegion(bool editor) override { return editor ? RefreshEditorPreview():Refresh(); }
    void LeaveSpatialRegion() override;
    void OnDestroy() override;
    bool RefreshEditorPreview();
    const Engine::Grass::GrassPerformance& GetLastPerformance() const
    { return m_performance; }

private:
    using Tile = std::pair<int, int>;
    using Source = std::vector<Engine::Components::Mesh::Vertex>;
    // Species 0 is shortMesh, 1 tallMesh, then each accent, then each shrub
    // (m_accentSpecies and m_shrubSpecies count the loaded ones). Every species owns
    // one vertex range of the shared per-LOD source mesh and one instance range
    // per tile. Unknown authored assets render as species 0.
    std::vector<std::string> m_speciesAssets;
    std::map<std::string, size_t> m_speciesIndex;
    // [species][lod]
    std::vector<std::vector<std::shared_ptr<const Source>>> m_speciesVertices;
    std::vector<std::shared_ptr<Engine::Components::Mesh>> m_sourceMeshes;
    Engine::Graphics::IGraphicsBufferFactory* m_loadedFactory = nullptr;
    glm::vec3 m_sourceMinimum{0}, m_sourceMaximum{0};
    using Placement = Engine::Components::TerrainObjectLayer::Placement;
    using Clock = std::chrono::steady_clock;
    struct TileState
    {
        Engine::Core::Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        Engine::Components::Material* material = nullptr; // the tile object's own
        uint64_t desiredRefresh = 0; // m_refreshSerial of the last Refresh that wanted it
        int desiredLod = 0;
        bool castsShadows = false;
        float distanceSquared = 0;
        float visibility = 0;
        float appearance = 0;
        // Ground height at the tile centre (terrain-local). The tile object
        // sits there and its placements are stored relative to it, so the
        // tile origin that LOD selection and fades measure from is on the
        // ground among its plants, not at height 0 (tens to hundreds of
        // metres away on high terrain, which put every plant in LOD2).
        float originHeight = 0;
    };
    struct BuildJob
    {
        Tile tile;
        int lod = 0;
        uint64_t signature = 0;
        std::vector<Placement> placements;
        size_t placement = 0;
        Engine::Components::MeshInstanceLayerData instanceLayer;
        std::vector<uint32_t> speciesCounts;
        uint32_t totalCount=0;
        bool prepared=false;
        glm::vec3 minimum{0}, maximum{0};
    };
    std::map<Tile, TileState> m_tiles;
    const Engine::Components::TerrainObjectLayer::PreparedTiles* m_placementTiles=nullptr;
    std::optional<BuildJob> m_build;
    const Engine::Components::TerrainObjectLayer* m_indexedLayer = nullptr;
    uint64_t m_placementRevision = 0;
    const Placement* m_placementData = nullptr;
    size_t m_placementCount = 0;
    float m_indexedTileSize = 0;
    uint64_t m_placementSignature = 0;
    Engine::Grass::GrassPerformance m_performance;
    bool m_loaded = false;
    std::string m_loadedMaterialPath, m_resolvedMaterialPath;
    Clock::time_point m_lastRefresh{};
    bool m_authoredPreviewHidden = false;
    uint64_t m_lastGenerationSignature = 0;
    uint64_t m_refreshSerial = 0;
    bool m_hasGenerationSignature = false;
    // [species][lod]
    std::vector<std::vector<std::string>> m_loadedSpeciesPaths;
    size_t m_accentSpecies = 0, m_shrubSpecies = 0, m_bushSpecies = 0;
    // Accent and shrub LOD chains, re-resolved only when their lists change.
    struct LodChains
    {
        std::string source;
        bool resolved = false;
        std::vector<std::vector<std::string>> chains;
    };
    mutable LodChains m_accentLods, m_shrubLods, m_bushLods;
    const std::vector<std::vector<std::string>>& ResolveLodChains(
        const std::string& list, LodChains& cache) const;
    const std::vector<std::vector<std::string>>& AccentLods() const
    { return ResolveLodChains(accentMeshes, m_accentLods); }
    const std::vector<std::vector<std::string>>& ShrubLods() const
    { return ResolveLodChains(shrubMeshes, m_shrubLods); }
    const std::vector<std::vector<std::string>>& BushLods() const
    { return ResolveLodChains(bushMeshes, m_bushLods); }
    // 0 outside thickets, 1 at their heart (terrain-local metres).
    float ShrubThicket(float x, float z) const;
    // The same thicket noise for bush clusters.
    float BushCluster(float x, float z) const;
    float ThicketNoise(float x, float z, float size, float coverage, uint32_t salt) const;
    size_t SpeciesOf(const std::string& asset) const;
    Engine::Core::Object* m_cachedViewer = nullptr;
    Engine::Components::Terrain* m_cachedTerrain = nullptr;
    Engine::Components::TerrainObjectLayer* m_cachedLayer = nullptr;
    Engine::Scene::Scene* m_cachedScene = nullptr;
    std::string m_cachedViewerName;
    uint64_t m_cachedStructureRevision = 0;

    // Ground tint (Terrain::GroundCover): mean grass colour per mesh pair.
    std::string m_coverColorSource;
    glm::vec3 m_coverColor{0.f};
    float GrassRadius() const;
    void PublishGroundCover();

    std::vector<float> m_lodDistances;
    bool m_useGpuLodSelection = false;
    bool LoadSources();
    bool Refresh();
    bool ResolveCachedReferences();
    bool EnsureAuthoredPreviewHidden();
    void ClearTiles();
    void SuspendTiles();
    void RefreshPlacementIndex();
    void BeginBuild(Tile tile, int lod, uint64_t signature);
    bool AdvanceBuild(Clock::time_point deadline);
    bool ActivateTileLod(TileState& tile, int lod);
    uint64_t ComputeGenerationSignature() const;

    float SampleHeight(float localX, float localZ, glm::vec3* normal = nullptr) const;
};
