#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include "Scripts/TerrainPerformance/TerrainHeightArchive.h"
#include <string>
#include <cstdint>
#include <vector>

namespace Engine::Components { class Mesh; }
namespace Engine::Core { class Object; }

// Streams a finite authored terrain world from its saved height archive.
class TerrainPerformanceDriver final : public Engine::Core::Script
{
public:
    TerrainPerformanceDriver();
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string viewerObjectName = "Terrain Performance Camera";
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance", ClampMin = "1")
    int mapHalfExtentCells = 64;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string cacheDirectory = ".local/cache/terrain_performance";
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string heightArchive =
        "Assets/TerrainData/TerrainPerformance/world.heights";
    // Saved with the authored map so play mode can load baked cells without
    // shipping the source height archive.
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string archiveSignature;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string farGrid =
        "Assets/TerrainData/TerrainPerformance/world.fargrid";
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    bool enableFarTerrain = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    float reportIntervalSeconds = 5.f;
    void Update() override;
    void OnAfterDeserialize(IGraphicsProvider* provider) override;
    uint64_t SourceSignature() const;
    bool ConfigureTerrain();
    std::vector<Engine::Components::Mesh*> GetFarTileMeshes() const;
    uint64_t GetFarTileUploadCount() const { return m_farTileUploadCount; }
    uint64_t GetFarTileUploadedBytes() const { return m_farTileUploadedBytes; }
    double GetFarTileRebuildMilliseconds() const { return m_farTileRebuildMilliseconds; }
private:
    struct FarTile
    {
        int startX = 0, startY = 0, endX = 0, endY = 0;
        Engine::Core::Object* object = nullptr;
        Engine::Components::Mesh* mesh = nullptr;
    };
    float m_reportElapsed = 0.f;
    size_t m_peakLoaded = 0;
    uint64_t m_configuredSignature = 0;
    bool m_previewLoaded = false;
    std::vector<float> m_farHeights;
    std::vector<TerrainHeightArchive::FarPatch> m_farPatches;
    std::vector<FarTile> m_farTiles;
    uint64_t m_farTileUploadCount = 0;
    uint64_t m_farTileUploadedBytes = 0;
    double m_farTileRebuildMilliseconds = 0.0;
    int m_farCenterX = 0, m_farCenterY = 0;
    bool m_farCenterValid = false;
    bool m_farAttempted = false;
    void RefreshFarTerrain(int centerX, int centerY);
};
