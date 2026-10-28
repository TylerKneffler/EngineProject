#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include "Core/Compoonents/Obj/TerrainHeightArchive.h"
#include <future>
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
    uint64_t GetFarTileUploadCount() const;
    uint64_t GetFarTileUploadedBytes() const;
    double GetFarTileRebuildMilliseconds() const;
private:
    struct FarGridLoad
    {
        bool loaded = false;
        std::vector<float> heights;
        std::vector<TerrainHeightArchive::FarPatch> patches;
    };
    static FarGridLoad LoadFarGridData(const std::string& path,
        const std::string& archivePath, const std::string& editDirectory,
        const std::vector<std::pair<int, int>>& editedCells,
        uint64_t signature, int extent, int cellsX, int cellsY);
    std::future<FarGridLoad> m_farLoad;
    float m_reportElapsed = 0.f;
    size_t m_peakLoaded = 0;
    uint64_t m_configuredSignature = 0;
    bool m_previewLoaded = false;
    bool m_farAttempted = false;
};
