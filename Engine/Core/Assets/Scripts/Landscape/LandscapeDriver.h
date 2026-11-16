#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include "Core/Compoonents/Terrain/TerrainHeightArchive.h"
#include <cstdint>
#include <glm/glm.hpp>
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
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string farGrid = "Assets/TerrainData/Landscape/world.fargrid";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string cacheDirectory = ".local/cache/landscape_demo";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    bool enableFarTerrain = true;
    // Height of the (future) lake surface; the generator carves basins below it.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    float waterLevel = -40.f;
    // Trees tried per fully forested 128 m cell.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "0", ClampMax = "600")
    int treesPerCell = 220;
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string forestLayerName = "Landscape forest";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "4", ClampMax = "80")
    float grassRadius = 26.f;
    // Terrain object layer (listed in the TerrainManager) that receives
    // rocks, fallen logs and bushes; empty disables props. A non-empty
    // undergrowthLayerName moves the bushes to a layer of their own, at the
    // cost of one more layer per cell.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string propsLayerName = "Landscape props";
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape")
    std::string undergrowthLayerName;
    // Multiplier on rock, log and bush attempts per cell.
    PROPERTY(Inspector, EditAnywhere, Category = "Landscape", ClampMin = "0", ClampMax = "4")
    float propsDensity = 1.f;
    // A flat water plane at waterLevel over the whole map fills the lakes.
    PROPERTY(Inspector, EditAnywhere, Category = "Water")
    bool enableWater = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Water")
    std::string waterMaterial = "Engine/Core/Assets/Materials/Water/LakeWater.material";

    void Start() override;
    void Update() override;
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
    // Creates, updates or removes the lake water plane.
    void UpdateWater();
    uint64_t m_waterObjectId = 0;
    uint64_t m_waterSignature = 0;
    std::future<FarGridLoad> m_farLoad;
    uint64_t m_configuredSignature = 0;
    bool m_previewLoaded = false;
    bool m_farAttempted = false;
    bool m_farDataReady = false;
};
