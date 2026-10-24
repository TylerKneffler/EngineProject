#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Object.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::string scenePath =
        "Engine/Core/Assets/Scenes/terrain/terrain_performance.scene";
    std::string cacheDirectory;
    int limit = -1;
    bool force = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--force") force = true;
        else if ((option == "--scene" || option == "--output" || option == "--limit") &&
            i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (option == "--scene") scenePath = value;
            else if (option == "--output") cacheDirectory = value;
            else
            {
                try { limit = std::stoi(value); }
                catch (...) { std::cerr << "Invalid --limit value\n"; return 2; }
                if (limit < 1) { std::cerr << "--limit must be positive\n"; return 2; }
            }
        }
        else
        {
            std::cerr << "Usage: TerrainCacheBake [--scene path] [--output directory] "
                "[--limit count] [--force]\n";
            return 2;
        }
    }
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene, scenePath, nullptr))
    {
        std::cerr << "Could not load scene: " << scenePath << '\n';
        return 1;
    }
    auto* object = scene.FindObjectByName("Terrain Performance Map");
    auto* manager = object ? object->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    auto* driver = object ? object->GetComponent<TerrainPerformanceDriver>() : nullptr;
    auto* noise = object ? object->GetComponent<PerlinNoiseField>() : nullptr;
    if (!manager || !driver || !noise)
    {
        std::cerr << "Scene has no terrain performance map, driver, or noise source\n";
        return 1;
    }
    if (!cacheDirectory.empty()) driver->cacheDirectory = cacheDirectory;
    driver->Update(); // Configures deterministic generation and the cache signature.
    const int extent = std::max(0, driver->mapHalfExtentCells);
    const int side = extent * 2 + 1;
    const int total = side * side;
    const int target = limit < 0 ? total : std::min(limit, total);
    int completed = 0;
    for (int z = -extent; z <= extent && completed < target; ++z)
        for (int x = -extent; x <= extent && completed < target; ++x)
        {
            if (!manager->BakeProceduralCell(x, z, force))
            {
                std::cerr << "Failed to bake cell " << x << ',' << z << '\n';
                return 1;
            }
            ++completed;
            if (completed % 256 == 0 || completed == target)
                std::cout << "Baked " << completed << '/' << total << " cells\n";
        }
    return 0;
}
