#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

// Builds a finite large world lazily around a moving viewer. Each cell samples
// the same world-space noise field, so generated borders agree in any order.
class TerrainPerformanceDriver final : public Engine::Core::Script
{
public:
    TerrainPerformanceDriver();
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string viewerObjectName = "Terrain Performance Camera";
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance", ClampMin = "1")
    int mapHalfExtentCells = 64;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance", ClampMin = "0")
    int generationRadiusCells = 9;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance", ClampMin = "1")
    int cellsPerUpdate = 2;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance", ClampMin = "1")
    int maxGeneratedCells = 20000;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    std::string cacheDirectory = ".local/cache/terrain_performance";
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    float heightAmplitude = 38.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    float baseHeight = -8.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Performance")
    float reportIntervalSeconds = 5.f;
    void Update() override;
private:
    float m_reportElapsed = 0.f;
    size_t m_peakLoaded = 0;
    double m_peakGenerationMs = 0.0;
};
