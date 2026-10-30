#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include "Core/Compoonents/Obj/Mesh.h"
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

// First grass rendering slice: deterministic, near-viewer tile meshes made
// from reusable blade patches. Tiles are CPU-batched to keep draw count low.
class GrassField final : public Engine::Core::Script
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
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Tiles", ClampMin = "1", ClampMax = "16")
    int maxTileBuildsPerUpdate = 4;
    PROPERTY(Inspector, EditAnywhere, Category = "Grass | Lighting")
    bool castShadows = false;

    void Start() override;
    void Update() override;
    void OnDestroy() override;
    bool RefreshEditorPreview();

private:
    using Tile = std::pair<int, int>;
    using Source = std::vector<Engine::Components::Mesh::Vertex>;
    std::array<Source, 3> m_shortVertices;
    std::array<Source, 3> m_tallVertices;
    struct TileState
    {
        Engine::Core::Object* object = nullptr;
        int lod = 0;
        bool castsShadows = false;
        uint64_t geometrySignature = 0;
    };
    struct PendingTileUpdate
    {
        int lod = 0;
        bool castsShadows = false;
        uint64_t geometrySignature = 0;
        float distanceSquared = 0.f;
        uint64_t refreshEpoch = 0;
    };
    std::map<Tile, TileState> m_tiles;
    std::map<Tile, PendingTileUpdate> m_pendingTileUpdates;
    bool m_loaded = false;
    bool m_authoredPreviewHidden = false;
    uint64_t m_lastGenerationSignature = 0;
    bool m_hasGenerationSignature = false;
    uint64_t m_refreshEpoch = 0;
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
    bool BuildTile(Tile tile, int lod, bool castsTileShadows,
        uint64_t geometrySignature);
    uint64_t ComputeGenerationSignature() const;
    uint64_t ComputeTileGeometrySignature(Tile tile) const;
    float SampleHeight(float localX, float localZ) const;
};
