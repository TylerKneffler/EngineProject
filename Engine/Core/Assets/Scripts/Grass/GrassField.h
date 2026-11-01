#pragma once

#include "Core/Script.h"
#include "Core/Compoonents/SpatialUpdate.h"
#include "Core/PropertyMacros.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Rendering/GrassPerformance.h"
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
class Terrain;
class TerrainObjectLayer;
}

namespace Engine::Scene { class Scene; }

// Deterministic tiles sharing species geometry. Only compact placement records
// are prepared on the CPU; all blade vertex transforms run on the GPU.
class GrassField final : public Engine::Core::Script, public Engine::Components::SpatialUpdate
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
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "1")
    float tileSize = 4.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "0")
    float shortPatchesPerSquareMeter = .5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "0")
    float tallPatchesPerSquareMeter = .16f;
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
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0")
    float fadeDuration = .35f;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD")
    bool gpuLodSelection = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles")
    bool allowProceduralGeneration = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | LOD", ClampMin = "0")
    float distanceFadeWidth = 2.f;

    void Start() override;
    void Update() override;
    bool RequiresUpdate() const override { return false; }
    uint64_t SpatialRegionRevision() const override;
    Engine::Components::SpatialUpdateRegion DescribeSpatialRegion() override;
    bool TickSpatialRegion(bool editor) override { return editor ? RefreshEditorPreview():Refresh(); }
    void LeaveSpatialRegion() override { ClearTiles(); m_performance={}; }
    void OnDestroy() override;
    bool RefreshEditorPreview();
    const Engine::Rendering::GrassPerformance& GetLastPerformance() const
    { return m_performance; }

private:
    using Tile = std::pair<int, int>;
    using Source = std::vector<Engine::Components::Mesh::Vertex>;
    std::array<std::shared_ptr<const Source>, 3> m_shortVertices;
    std::array<std::shared_ptr<const Source>, 3> m_tallVertices;
    std::array<std::shared_ptr<Engine::Components::Mesh>, 3> m_sourceMeshes;
    Engine::Graphics::IGraphicsBufferFactory* m_loadedFactory = nullptr;
    glm::vec3 m_sourceMinimum{0}, m_sourceMaximum{0};
    using Placement = Engine::Components::TerrainObjectLayer::Placement;
    using Clock = std::chrono::steady_clock;
    struct TileState
    {
        Engine::Core::Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
        int desiredLod = 0;
        bool castsShadows = false;
        float distanceSquared = 0;
        float visibility = 0;
        float appearance = 0;
    };
    struct BuildJob
    {
        Tile tile;
        int lod = 0;
        uint64_t signature = 0;
        std::vector<Placement> placements;
        size_t placement = 0;
        std::shared_ptr<Engine::Components::Mesh::InstanceData> instances;
        uint32_t shortCount = 0,totalCount=0;
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
    Engine::Rendering::GrassPerformance m_performance;
    bool m_loaded = false;
    Clock::time_point m_lastRefresh{};
    bool m_authoredPreviewHidden = false;
    uint64_t m_lastGenerationSignature = 0;
    bool m_hasGenerationSignature = false;
    std::array<std::string, 3> m_loadedShortPaths;
    std::array<std::string, 3> m_loadedTallPaths;
    Engine::Core::Object* m_cachedViewer = nullptr;
    Engine::Components::Terrain* m_cachedTerrain = nullptr;
    Engine::Components::TerrainObjectLayer* m_cachedLayer = nullptr;
    Engine::Scene::Scene* m_cachedScene = nullptr;
    std::string m_cachedViewerName;
    uint64_t m_cachedStructureRevision = 0;

    bool LoadSources();
    bool Refresh();
    bool ResolveCachedReferences();
    bool EnsureAuthoredPreviewHidden();
    void ClearTiles();
    void RefreshPlacementIndex();
    void BeginBuild(Tile tile, int lod, uint64_t signature);
    bool AdvanceBuild(Clock::time_point deadline);
    bool ActivateTileLod(TileState& tile, int lod);
    uint64_t ComputeGenerationSignature() const;

    float SampleHeight(float localX, float localZ) const;
};
