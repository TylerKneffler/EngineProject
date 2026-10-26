#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainPerformance/TerrainHeightArchive.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Object.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
bool BakeHeightArchive(const std::string& path, uint64_t signature,
    int extent, const Engine::Components::TerrainManager& manager,
    const TerrainPerformanceDriver& driver, const PerlinNoiseField& noise,
    bool force)
{
    std::vector<float> existing;
    std::error_code existingError;
    if (!force && std::filesystem::file_size(path, existingError) ==
        TerrainHeightArchive::ExpectedBytes(extent, manager.cellsX,
            manager.cellsY) && !existingError &&
        TerrainHeightArchive::ReadCell(path, signature, extent,
            manager.cellsX, manager.cellsY, 0, 0, existing)) return true;
    const std::filesystem::path target(path);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const std::string temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file || !TerrainHeightArchive::BeginWrite(file, signature, extent,
        manager.cellsX, manager.cellsY)) return false;
    std::vector<int16_t> samples(static_cast<size_t>(manager.cellsX + 1) *
        (manager.cellsY + 1));
    std::vector<uint8_t> packed((samples.size() + 3) / 4);
    for (int z = -extent; z <= extent; ++z)
        for (int x = -extent; x <= extent; ++x)
        {
            std::fill(packed.begin(), packed.end(), uint8_t{0});
            for (int row = 0; row <= manager.cellsY; ++row)
                for (int col = 0; col <= manager.cellsX; ++col)
                {
                    const double worldX = (static_cast<double>(x) * manager.cellsX + col) *
                        manager.cellWidth;
                    const double worldZ = (static_cast<double>(z) * manager.cellsY + row) *
                        manager.cellDepth;
                    const float height = driver.baseHeight + driver.heightAmplitude *
                        noise.SampleFractal2D(worldX, worldZ);
                    const size_t sample =
                        static_cast<size_t>(row * (manager.cellsX + 1) + col);
                    samples[sample] = TerrainHeightArchive::Quantize(height);
                    const float savedHeight = static_cast<float>(samples[sample]) /
                        TerrainHeightArchive::Scale;
                    packed[sample / 4] |= TerrainHeightArchive::LayerForHeight(savedHeight)
                        << ((sample % 4) * 2);
                }
            file.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
            file.write(reinterpret_cast<const char*>(packed.data()),
                static_cast<std::streamsize>(packed.size()));
            if (!file) return false;
        }
    file.close();
    if (!file) return false;
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    return !error;
}
}

int main(int argc, char** argv)
{
    std::string scenePath =
        "Engine/Core/Assets/Scenes/terrain/terrain_performance.scene";
    std::string cacheDirectory;
    std::string heightPath;
    int limit = -1;
    bool force = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--force") force = true;
        else if ((option == "--scene" || option == "--output" ||
            option == "--heights" || option == "--limit") &&
            i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (option == "--scene") scenePath = value;
            else if (option == "--output") cacheDirectory = value;
            else if (option == "--heights") heightPath = value;
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
                "[--heights file] "
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
    if (heightPath.empty())
    {
        heightPath = driver->heightArchive;
        const std::string prefix = "Assets/";
        if (heightPath.rfind(prefix, 0) == 0)
            heightPath = "Engine/Core/Assets/" + heightPath.substr(prefix.size());
    }
    const int extent = std::max(0, driver->mapHalfExtentCells);
    if (!BakeHeightArchive(heightPath, driver->SourceSignature(), extent,
        *manager, *driver, *noise, force))
    {
        std::cerr << "Could not bake saved height archive: " << heightPath << '\n';
        return 1;
    }
    std::cout << "Saved terrain heights: " << heightPath << '\n';
    std::vector<std::pair<int, int>> editedCells;
    const auto& edits = manager->Serialize()["editedCells"];
    for (size_t i = 0; i < edits.ArraySize(); ++i)
    {
        const auto& cell = edits.ArrayAt(i);
        editedCells.emplace_back(cell["x"].AsInt(), cell["y"].AsInt());
    }
    const auto farGridPath = std::filesystem::path(heightPath)
        .replace_extension(".fargrid").string();
    if (!TerrainHeightArchive::BakeFarGrid(heightPath, farGridPath,
        driver->SourceSignature(), extent, manager->cellsX, manager->cellsY,
        manager->cellDataDirectory, editedCells))
    {
        std::cerr << "Could not bake distant terrain grid: " << farGridPath << '\n';
        return 1;
    }
    std::cout << "Saved distant terrain grid: " << farGridPath << '\n';
    driver->heightArchive = heightPath;
    driver->farGrid = farGridPath;
    if (!driver->ConfigureTerrain())
    {
        std::cerr << "Could not configure saved terrain source\n";
        return 1;
    }
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
