#pragma once
#include "Core/Components/Terrain/TerrainPaintArchive.h"
#include "Core/Components/Script.h"
#include "Core/Components/PropertyMacros.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <functional>
#include <future>
#include <string>
#include <vector>


// Drives the landscape demo map (mountains, forests, lake basins) on a
// TerrainManager. The map is a height archive written by
// tools/build_landscape_demo.py; this script streams it, paints the six PBR
// terrain layers from height, the archive's rock class and a forest mask,
// grows oak forests, and limits procedural grass to the grass layer.
//
// Layers: 0 sand (lake beds), 1 gravel (shores), 2 meadow grass,
// 3 forest soil, 4 rock, 5 snow. Lakes have no water yet: everything below
// waterLevel is a sand bed ringed by gravel.
class LandscapeDriver final : public Engine::Core::Script
{
public:
    LandscapeDriver();
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string viewerObjectName = "Landscape Explorer";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "1")
    int mapHalfExtentCells = 20;
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string heightArchive = "Assets/TerrainData/Landscape/world.heights";
    // The map's baked material paint (TerrainPaintArchive, BakePaint): the
    // ground's paint as loaded; hand paint goes on top.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string paintArchive = "Assets/TerrainData/Landscape/world.paint";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string farGrid = "Assets/TerrainData/Landscape/world.fargrid";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string cacheDirectory = ".local/cache/landscape_demo";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    bool enableFarTerrain = true;
    // Height of the (future) lake surface; the generator carves basins below it.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    float waterLevel = -40.f;
    // Heights where rock, then snow, take over regardless of slope, and the
    // top of the forests. The defaults suit a map whose ranges reach 120 m.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    float rockLine = 58.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    float snowLine = 102.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    float treeLine = 46.f;
    // Widens the rock, snow and tree-line transitions (1 = 16 m rock band);
    // scale it with the map's relief.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "0.1", ClampMax = "50")
    float altitudeBlend = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string forestLayerName = "Landscape forest";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "4", ClampMax = "80")
    float grassRadius = 26.f;
    // A flat water plane at waterLevel over the whole map fills the lakes.
    PROPERTY(Inspector, EditAnywhere, Category = "Water")
    bool enableWater = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Water")
    std::string waterMaterial = "Engine/Core/Assets/Materials/Water/LakeWater.material";
    // Macro color map for distant terrain (TerrainManager::macroColorMap):
    // the cells' paint (baked, then hand paint) at 16 m, with the canopy of
    // the saved trees.
    PROPERTY(Inspector, EditAnywhere, Category = "Macro color")
    bool enableMacroColors = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Macro color", ClampMin = "0.1", ClampMax = "2")
    float macroBrightness = .85f;

    void Start() override;
    void Update() override;
    void OnDestroy() override;
    void FenceBackgroundWork() override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    bool ConfigureTerrain();

private:
    struct FarGridLoad
    {
        bool loaded = false;
        std::vector<float> heights;
        std::vector<TerrainHeightArchive::FarPatch> patches;
        // Far-tile color per cell corner (see FarTerrainPalette::gridColors).
        std::vector<glm::vec3> gridColors;
    };
    uint64_t SourceSignature() const;
    // Registers the macro color composer with the TerrainManager.
    void RegisterMacroColors(Engine::Components::TerrainManager& manager);
    // Recomposes edited cells of the macro map from the terrain as it is
    // (TerrainManager::MacroColorSource::makeCellUpdate): heights from the
    // field (sculpts and volume edits), rock class from the surface where it
    // left the archive, the cells' erosion and hand paint, and the trees
    // around them for the canopy.
    std::function<std::vector<Engine::Components::TerrainManager::MacroColorPatch>()>
    MakeMacroCellUpdate(Engine::Components::TerrainManager& manager,
        const std::vector<std::pair<int, int>>& cells,
        const Engine::Components::TerrainManager::MacroColorImage& map);
    uint64_t MacroColorRevision() const;
    // Creates, updates or removes the lake water plane.
    void UpdateWater();
    uint64_t m_waterObjectId = 0;
    uint64_t m_waterSignature = 0;
    std::future<FarGridLoad> m_farLoad;
    uint64_t m_configuredSignature = 0;
    // The baked paint (paintArchive), opened by ConfigureTerrain.
    std::shared_ptr<const Engine::Components::TerrainPaintArchive> m_paint;
    bool m_previewLoaded = false;
    bool m_farAttempted = false;
    bool m_farDataReady = false;
};
