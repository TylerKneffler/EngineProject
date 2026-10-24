#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

TerrainPerformanceDriver::TerrainPerformanceDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainPerformanceDriver));
    RegisterField("viewerObjectName", viewerObjectName);
    RegisterField("mapHalfExtentCells", mapHalfExtentCells);
    RegisterField("generationRadiusCells", generationRadiusCells);
    RegisterField("cellsPerUpdate", cellsPerUpdate);
    RegisterField("maxGeneratedCells", maxGeneratedCells);
    RegisterField("cacheDirectory", cacheDirectory);
    RegisterField("heightAmplitude", heightAmplitude);
    RegisterField("baseHeight", baseHeight);
    RegisterField("reportIntervalSeconds", reportIntervalSeconds);
}

namespace
{
struct TerrainPerformanceDriverRegistration
{
    TerrainPerformanceDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainPerformanceDriver>(
            "TerrainPerformanceDriver");
    }
} registration;
}

void TerrainPerformanceDriver::Update()
{
    if (!Owner) return;
    auto* manager = Owner->GetComponent<Engine::Components::TerrainManager>();
    auto* noise = Owner->GetComponent<PerlinNoiseField>();
    auto* viewer = Owner->FindObjectInSceneByName(viewerObjectName);
    if (!manager || !noise || !viewer) return;
    uint64_t signature = 14695981039346656037ull;
    const auto mix = [&signature](const auto& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i)
            signature = (signature ^ bytes[i]) * 1099511628211ull;
    };
    const uint32_t generatorVersion = 1;
    mix(generatorVersion);
    mix(manager->cellsX); mix(manager->cellsY);
    mix(manager->cellWidth); mix(manager->cellDepth);
    mix(noise->seed); mix(noise->frequency); mix(noise->octaves);
    mix(noise->lacunarity); mix(noise->persistence);
    mix(noise->coordinateOffset.x); mix(noise->coordinateOffset.y);
    mix(noise->coordinateOffset.z);
    mix(baseHeight); mix(heightAmplitude);
    manager->SetStaticCache(cacheDirectory, signature);
    const int countX = manager->cellsX, countY = manager->cellsY;
    const float width = manager->cellWidth, depth = manager->cellDepth;
    const int seed = noise->seed, octaves = noise->octaves;
    const float frequency = noise->frequency, lacunarity = noise->lacunarity;
    const float persistence = noise->persistence;
    const glm::vec3 coordinateOffset = noise->coordinateOffset;
    const float heightBase = baseHeight, amplitude = heightAmplitude;
    manager->SetProceduralBuilder([=](Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain)
    {
        PerlinNoiseField generator;
        generator.seed = seed;
        generator.frequency = frequency;
        generator.octaves = octaves;
        generator.lacunarity = lacunarity;
        generator.persistence = persistence;
        generator.coordinateOffset = coordinateOffset;
        terrain.layers = {
            {"Assets/Materials/Terrain/dark_soil.material", -1000.f, -12.f,
                {.26f, .22f, .17f}},
            {"Assets/Materials/Terrain/warm_dirt.material", -12.f, 0.f,
                {.47f, .36f, .24f}},
            {"Assets/Materials/Terrain/meadow_grass.material", 0.f, 16.f,
                {.27f, .44f, .18f}},
            {"Assets/Materials/Terrain/highland_grass.material", 16.f, 1000.f,
                {.49f, .53f, .29f}}
        };
        for (int row = 0; row <= countY; ++row)
            for (int col = 0; col <= countX; ++col)
            {
                const double worldX = (static_cast<double>(cell.first) * countX + col) * width;
                const double worldZ = (static_cast<double>(cell.second) * countY + row) * depth;
                terrain.heights[static_cast<size_t>(row * (countX + 1) + col)] =
                    heightBase + amplitude * generator.SampleFractal2D(worldX, worldZ);
            }
        terrain.AssignLayersByHeight();
    });
    const glm::vec3 local = glm::vec3(glm::inverse(Owner->transform.GetWorldMatrix()) *
        glm::vec4(viewer->transform.GetWorldMatrix()[3]));
    const float spanX = manager->cellsX * manager->cellWidth;
    const float spanZ = manager->cellsY * manager->cellDepth;
    const int centerX = static_cast<int>(std::floor(local.x / spanX));
    const int centerZ = static_cast<int>(std::floor(local.z / spanZ));
    const int radius = std::clamp(generationRadiusCells, 0, 32);
    const auto generationStart = std::chrono::steady_clock::now();
    int created = 0;
    for (int ring = 0; ring <= radius && created < std::max(1, cellsPerUpdate); ++ring)
        for (int dz = -ring; dz <= ring && created < std::max(1, cellsPerUpdate); ++dz)
            for (int dx = -ring; dx <= ring && created < std::max(1, cellsPerUpdate); ++dx)
            {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                const int cx = centerX + dx, cz = centerZ + dz;
                if (std::abs(cx) > mapHalfExtentCells ||
                    std::abs(cz) > mapHalfExtentCells ||
                    manager->HasCell(cx, cz) ||
                    manager->GetCellCount() >= static_cast<size_t>(std::max(1, maxGeneratedCells)))
                    continue;
                if (!manager->AddProceduralCell(cx, cz)) continue;
                ++created;
            }
    const double generationMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - generationStart).count();
    m_peakGenerationMs = std::max(m_peakGenerationMs, generationMs);
    m_peakLoaded = std::max(m_peakLoaded, manager->GetLoadedCellCount());
    m_reportElapsed += Owner->GetScene()
        ? std::max(0.f, Owner->GetScene()->GetDeltaTime()) : 0.f;
    if (m_reportElapsed >= std::max(.1f, reportIntervalSeconds))
    {
        std::cout << "[Terrain performance] generated=" << manager->GetCellCount()
            << " loaded=" << manager->GetLoadedCellCount()
            << " peakLoaded=" << m_peakLoaded
            << " nearRadiusCells=" << manager->loadedRadius
            << " farRadiusCells=" << std::max(manager->loadedRadius, manager->farRadius)
            << " viewDistanceMeters=" << std::max(manager->loadedRadius,
                manager->farRadius) * std::max(spanX, spanZ)
            << " peakGenerationMs=" << m_peakGenerationMs << '\n';
        m_reportElapsed = 0.f;
    }
}
