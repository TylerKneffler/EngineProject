#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Serialization/SceneSerializer.h"
#include <chrono>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <glm/gtc/constants.hpp>
#include <iostream>

static void RequireAt(bool condition, int line)
{
    if (!condition)
    {
        std::cerr << "Terrain manager regression failed at line " << line << '\n';
        throw std::runtime_error("Terrain manager regression failed");
    }
}
#define Require(condition) RequireAt((condition), __LINE__)

int main()
{
    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Terrain manager");
    auto* manager = object->AddComponent<Engine::Components::TerrainManager>();
    manager->cellsX = 2; manager->cellsY = 2;
    manager->cellWidth = 1.f; manager->cellDepth = 1.f;
    Require(manager->AddCell(0, 0));
    Require(manager->ApplyBrush(2.f, 1.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(manager->AddCell(1, 0));
    auto* left = manager->GetLoadedCell(0, 0);
    auto* right = manager->GetLoadedCell(1, 0);
    Require(left && right);
    Require(left->heights[5] == right->heights[3]);
    Require(manager->ApplyBrush(2.f, 1.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(left->heights[5] == right->heights[3]);
    Require(left->heights[5] == 2.f);
    const auto saved = manager->Serialize();
    manager->loadedRadius = 0;
    manager->SyncLoadedCells(0, 0);
    Require(manager->GetLoadedCell(1, 0) == nullptr);
    manager->SyncLoadedCells(1, 0);
    Require(manager->GetLoadedCell(1, 0)->heights[3] == 2.f);
    Require(manager->GetLoadedCell(0, 0) == nullptr);
    manager->Deserialize(saved);
    manager->OnAfterDeserialize(nullptr);
    Require(manager->HasCell(0, 0) && manager->HasCell(1, 0));
    Require(manager->GetLoadedCell(0, 0)->heights[5] == 2.f);
    Require(manager->RemoveCell(1, 0));
    Require(!manager->HasCell(1, 0));

    auto* largeObject = scene.AddObject("Large coordinate map");
    auto* large = largeObject->AddComponent<Engine::Components::TerrainManager>();
    large->loadedRadius = 0;
    large->SyncLoadedCells(10000, 10000);
    large->SetProceduralBuilder([](Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain&) {});
    for (int row = 0; row < 120; ++row)
        for (int col = 0; col < 120; ++col)
            Require(large->AddProceduralCell(col, row));
    Require(large->GetCellCount() == 14400);
    Require(large->GetStoredTerrainCount() == 0);
    Require(large->GetLoadedCellCount() == 0);
    const auto largeManifest = large->Serialize();
    Require(largeManifest["proceduralRuns"].ArraySize() == 120);
    Require(largeManifest["cells"].ArraySize() == 0);

    auto* budgetObject = scene.AddObject("Streaming budget");
    auto* budget = budgetObject->AddComponent<Engine::Components::TerrainManager>();
    budget->cellsX = budget->cellsY = 2;
    budget->maxLoadsPerUpdate = 2;
    budget->maxUnloadsPerUpdate = 1;
    budget->loadedRadius = 1;
    budget->SetProceduralBuilder([](Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain&) {});
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            Require(budget->AddProceduralCell(x, y));
    budget->UpdateStreaming(0, 0);
    Require(budget->GetLoadedCellCount() == 2);
    budget->UpdateStreaming(0, 0);
    Require(budget->GetLoadedCellCount() == 4);
    budget->UpdateStreaming(100, 100);
    Require(budget->GetLoadedCellCount() == 3);
    budget->editorRadius = 0;
    for (int step = 0; step < 4; ++step)
        budget->UpdateEditorStreaming({0.5, 0.0, 0.5});
    Require(budget->GetLoadedCellCount() == 1);
    Require(budget->GetLoadedCell(0, 0) != nullptr);
    Require(budget->GetLoadedCell(0, 0)->GetMeshLodStride() == 1);
    Require(budget->Serialize()["editorRadius"].AsInt() == 0);
    budget->UpdateStreaming(1, 1);
    Require(budget->GetLoadedCellCount() == 3);

    auto* asyncObject = scene.AddObject("Async streaming");
    auto* async = asyncObject->AddComponent<Engine::Components::TerrainManager>();
    async->cellsX = async->cellsY = 2;
    async->loadedRadius = 1;
    async->asyncStreaming = true;
    async->maxPreparedCells = 2;
    async->maxLoadsPerUpdate = 2;
    async->maxUploadsPerUpdate = 1;
    const auto asyncCachePath = std::filesystem::temp_directory_path() /
        ("terrain_async_cache_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    async->SetStaticCache(asyncCachePath.string(), 91);
    auto buildCount = std::make_shared<std::atomic<int>>(0);
    auto buildOrder = std::make_shared<std::vector<int>>();
    auto buildMutex = std::make_shared<std::mutex>();
    async->SetProceduralBuilder([buildCount, buildOrder, buildMutex](
        Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard<std::mutex> lock(*buildMutex);
            buildOrder->push_back(cell.first);
        }
        ++*buildCount;
        terrain.heights[4] = static_cast<float>(cell.first + 4);
        terrain.Rebuild();
    });
    for (int x = -1; x <= 1; ++x)
        Require(async->AddProceduralCell(x, 0));
    async->UpdateStreaming(-10, 0);
    async->UpdateStreaming(0, 0);
    Require(async->GetLoadedCellCount() == 0);
    Require(async->GetPreparingCellCount() == 2);
    for (int attempt = 0; attempt < 500 && async->GetLoadedCellCount() < 3; ++attempt)
    {
        const size_t before = async->GetLoadedCellCount();
        async->UpdateStreaming(0, 0);
        Require(async->GetLoadedCellCount() - before <= 1);
        Require(async->GetPreparingCellCount() <= 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCellCount() == 3);
    Require(buildCount->load() == 3);
    {
        std::lock_guard<std::mutex> lock(*buildMutex);
        Require((*buildOrder)[0] == 0 && (*buildOrder)[1] == 1);
    }
    Require(async->GetLoadedCell(1, 0)->heights[4] == 5.f);
    async->UpdateStreaming(100, 100);
    Require(async->GetLoadedCellCount() == 0);
    async->UpdateStreaming(-10, 0);
    for (int attempt = 0; attempt < 500 && async->GetLoadedCellCount() < 3; ++attempt)
    {
        async->UpdateStreaming(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCellCount() == 3);
    Require(buildCount->load() == 3);
    Require(async->GetCacheHitCount() == 3);
    const auto asyncEditPath = std::filesystem::temp_directory_path() /
        ("terrain_async_edits_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    async->cellDataDirectory = asyncEditPath.string();
    Require(async->ApplyBrush(3.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(async->GetLoadedCell(1, 0)->heights[4] == 6.f);
    async->UpdateStreaming(100, 100);
    Require(std::filesystem::exists(asyncEditPath / "1_0.terrainedit"));
    for (int attempt = 0; attempt < 500 && !async->GetLoadedCell(1, 0); ++attempt)
    {
        async->UpdateStreaming(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCell(1, 0) &&
        async->GetLoadedCell(1, 0)->heights[4] == 6.f);
    for (int attempt = 0; attempt < 500 && async->GetLoadedCellCount() < 3; ++attempt)
    {
        async->UpdateStreaming(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCellCount() == 3);
    async->loadedRadius = 0;
    async->farRadius = -1;
    async->UpdateStreaming(-1, 0);
    Require(async->GetLoadedCell(-1, 0) != nullptr);
    Require(async->GetLoadedCell(0, 0) == nullptr);
    Require(async->ApplyBrush(0.f, 1.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(async->GetLoadedCell(0, 0) != nullptr);
    const float asyncSeamHeight = async->GetLoadedCell(-1, 0)->heights[5];
    Require(async->GetLoadedCell(0, 0)->heights[3] == asyncSeamHeight);
    async->UpdateStreaming(100, 100);
    Require(std::filesystem::exists(asyncEditPath / "-1_0.terrainedit"));
    Require(std::filesystem::exists(asyncEditPath / "0_0.terrainedit"));
    for (int attempt = 0; attempt < 500 && !async->GetLoadedCell(-1, 0); ++attempt)
    {
        async->UpdateStreaming(-1, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCell(-1, 0) &&
        async->GetLoadedCell(-1, 0)->heights[5] == asyncSeamHeight);
    for (int attempt = 0; attempt < 500 && !async->GetLoadedCell(0, 0); ++attempt)
    {
        async->UpdateStreaming(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(async->GetLoadedCell(0, 0) &&
        async->GetLoadedCell(0, 0)->heights[3] == asyncSeamHeight);
    std::filesystem::remove_all(asyncCachePath);
    std::filesystem::remove_all(asyncEditPath);

    auto* prefetchObject = scene.AddObject("Approach prefetch");
    auto* prefetch = prefetchObject->AddComponent<Engine::Components::TerrainManager>();
    prefetch->cellsX = prefetch->cellsY = 2;
    prefetch->loadedRadius = 0;
    prefetch->asyncStreaming = true;
    prefetch->maxPreparedCells = 1;
    prefetch->maxUploadsPerUpdate = 1;
    std::atomic<int> aheadBuilds{0};
    prefetch->SetProceduralBuilder([&aheadBuilds](
        Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain) {
        if (cell.first == 1) ++aheadBuilds;
        terrain.Rebuild();
    });
    Require(prefetch->AddProceduralCell(0, 0));
    Require(prefetch->AddProceduralCell(1, 0));
    prefetch->SyncLoadedCells(0, 0);
    for (int attempt = 0; attempt < 500 && aheadBuilds.load() == 0; ++attempt)
    {
        prefetch->UpdateStreamingAtPosition({1.8, 0.0, 1.0});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(aheadBuilds.load() == 1);
    Require(prefetch->GetLoadedCell(1, 0) == nullptr);
    Require(prefetch->GetPreparingCellCount() == 1);
    for (int attempt = 0; attempt < 500 && !prefetch->GetLoadedCell(1, 0);
        ++attempt)
    {
        prefetch->UpdateStreamingAtPosition({2.1, 0.0, 1.0});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(prefetch->GetLoadedCell(1, 0) != nullptr);
    Require(aheadBuilds.load() == 1);

    Engine::Scene::Scene bakedScene;
    auto* bakedObject = bakedScene.AddObject("Baked terrain manager");
    auto* baked = bakedObject->AddComponent<Engine::Components::TerrainManager>();
    baked->cellsX = baked->cellsY = 2;
    baked->loadedRadius = 0;
    baked->SetStaticCache({}, 123);
    const auto runtimeBakePath = std::filesystem::temp_directory_path() /
        ("terrain_runtime_bake_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(runtimeBakePath);
    const auto bakedMaterialPath = runtimeBakePath / "paint.material";
    {
        std::ofstream material(bakedMaterialPath);
        material << "{\"version\":1,\"baseColor\":[0.8,0.2,0.1]}";
        Require(material.good());
    }
    baked->runtimeBakeDirectory = runtimeBakePath.string();
    baked->SetProceduralBuilder([&](Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain& terrain) {
        terrain.heights[4] = 7.f;
        terrain.layers.push_back({bakedMaterialPath.string(), 0.f, 10.f,
            {.8f, .2f, .1f}});
        terrain.weights.assign(terrain.heights.size() * 2, 0.f);
        for (size_t i = 0; i < terrain.heights.size(); ++i)
            terrain.weights[i * 2] = 1.f;
        terrain.weights[4 * 2] = .25f;
        terrain.weights[4 * 2 + 1] = .75f;
        terrain.Rebuild();
    });
    Require(baked->AddProceduralCell(0, 0));
    Require(baked->AddProceduralCell(1, 0));
    Require(!baked->HasCompleteRuntimeBake());
    Require(baked->BakeRuntimeTerrain());
    Require(baked->HasCompleteRuntimeBake());
    const auto originalMaterialTime =
        std::filesystem::last_write_time(bakedMaterialPath);
    std::filesystem::last_write_time(bakedMaterialPath,
        originalMaterialTime + std::chrono::seconds(5));
    Require(!baked->HasCompleteRuntimeBake());
    std::filesystem::last_write_time(bakedMaterialPath,
        originalMaterialTime);
    Require(baked->HasCompleteRuntimeBake());
    const auto packedBake = runtimeBakePath / "123.terrainpack";
    {
        std::fstream packed(packedBake,
            std::ios::binary | std::ios::in | std::ios::out);
        Require(static_cast<bool>(packed));
        const int32_t invalidX = 999;
        packed.seekp(24);
        packed.write(reinterpret_cast<const char*>(&invalidX),
            sizeof(invalidX));
        Require(static_cast<bool>(packed));
    }
    Require(!baked->HasCompleteRuntimeBake());
    {
        std::fstream packed(packedBake,
            std::ios::binary | std::ios::in | std::ios::out);
        const int32_t originalX = 0;
        packed.seekp(24);
        packed.write(reinterpret_cast<const char*>(&originalX),
            sizeof(originalX));
        Require(static_cast<bool>(packed));
    }
    Require(baked->HasCompleteRuntimeBake());
    Require(std::filesystem::exists(packedBake));
    Require(baked->Serialize()["runtimeBakeDirectory"].AsString() ==
        runtimeBakePath.string());
    baked->requireRuntimeBake = true;
    Require(baked->Serialize()["requireRuntimeBake"].AsBool());
    baked->SetProceduralBuilder([](Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain& terrain) {
        terrain.heights[4] = 99.f;
        terrain.Rebuild();
    });
    bakedScene.Start();
    std::vector<float> bakedSeamHeights, bakedSeamWeights;
    uint32_t bakedLayerCount = 0;
    Require(baked->ReadBakedCellHeights({1, 0}, bakedSeamHeights,
        &bakedSeamWeights, &bakedLayerCount));
    Require(bakedSeamHeights.size() == 9 && bakedSeamHeights[4] == 7.f);
    Require(bakedLayerCount == 2 && bakedSeamWeights.size() == 18 &&
        bakedSeamWeights[8] == .25f && bakedSeamWeights[9] == .75f);
    baked->UpdateStreaming(0, 0);
    Require(baked->GetLoadedCell(0, 0) &&
        baked->GetLoadedCell(0, 0)->heights[4] == 7.f);
    Require(baked->GetLoadedCell(0, 0)->GetBakedCpuMemoryBytes() > 0);
    baked->GetLoadedCell(0, 0)->SetMeshLodStride(2);
    Require(baked->GetLoadedCell(0, 0)->GetBakedCpuMemoryBytes() > 0);
    Require(baked->GetCacheHitCount() > 0);
    Require(std::filesystem::remove(packedBake));
    Require(!baked->HasCompleteRuntimeBake());
    baked->UpdateStreaming(1, 0);
    Require(baked->GetLoadedCell(1, 0) == nullptr);
    std::filesystem::remove_all(runtimeBakePath);

    Engine::Scene::Scene missingBakeScene;
    auto* missingObject = missingBakeScene.AddObject("Missing bake");
    auto* missingBake = missingObject->AddComponent<Engine::Components::TerrainManager>();
    missingBake->cellsX = missingBake->cellsY = 2;
    missingBake->loadedRadius = 0;
    missingBake->runtimeBakeDirectory = runtimeBakePath.string();
    missingBake->requireRuntimeBake = true;
    missingBake->SetStaticCache({}, 123);
    int unexpectedBuilds = 0;
    missingBake->SetProceduralBuilder([&](auto, auto& terrain) {
        ++unexpectedBuilds;
        terrain.Rebuild();
    });
    Require(missingBake->AddProceduralCell(0, 0));
    missingBakeScene.Start();
    Require(!missingBake->ReadBakedCellHeights({0, 0},
        bakedSeamHeights));
    missingBake->SyncLoadedCells(0, 0);
    Require(missingBake->GetLoadedCell(0, 0) == nullptr);
    Require(unexpectedBuilds == 0);

    Engine::Core::Object standaloneObject;
    auto* standalone = standaloneObject.AddComponent<Engine::Components::Terrain>();
    standalone->Resize(2, 2, 1.f, 1.f);
    standalone->heights[4] = 7.f;
    standalone->Rebuild();
    const auto standaloneBake = std::filesystem::temp_directory_path() /
        ("terrain_single_bake_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".terrainmesh");
    Require(standalone->SaveCache(standaloneBake.string(), 0, true));
    standalone->heights[4] = 99.f;
    standalone->Rebuild();
    standalone->bakedAssetPath = standaloneBake.string();
    standalone->Start();
    Require(standalone->heights[4] == 7.f);
    Require(standalone->GetBakedCpuMemoryBytes() > 0);
    std::filesystem::remove(standaloneBake);

    auto* lodObject = scene.AddObject("LOD borders");
    auto* lod = lodObject->AddComponent<Engine::Components::TerrainManager>();
    lod->cellsX = lod->cellsY = 32;
    lod->loadedRadius = 0;
    lod->farRadius = 2;
    lod->lod2Radius = lod->lod4Radius = 1;
    lod->SetProceduralBuilder([](Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain) {
        for (int y = 0; y <= terrain.cellsY; ++y)
            for (int x = 0; x <= terrain.cellsX; ++x)
            {
                const int worldX = cell.first * terrain.cellsX + x;
                terrain.heights[y * (terrain.cellsX + 1) + x] =
                    static_cast<float>((worldX * 7 + y * 11) % 17);
            }
        terrain.Rebuild();
    });
    for (int x = 0; x < 3; ++x) Require(lod->AddProceduralCell(x, 0));
    lod->SyncLoadedCells(0, 0);
    Require(lod->GetLoadedCell(0, 0)->GetMeshLodStride() == 1);
    Require(lod->GetLoadedCell(1, 0)->GetMeshLodStride() == 2);
    Require(lod->GetLoadedCell(2, 0)->GetMeshLodStride() == 8);
    auto* fine = lod->GetLoadedCell(1, 0)->Owner->
        GetComponent<Engine::Components::Mesh>();
    auto* coarse = lod->GetLoadedCell(2, 0)->Owner->
        GetComponent<Engine::Components::Mesh>();
    Require(coarse->GetVertexCount() < fine->GetVertexCount());
    // Every consecutive border sample must be an actual triangle edge at
    // both resolutions, so a neighbor cannot expose a T junction.
    auto checkBorder = [](const Engine::Components::Mesh& mesh, float x,
        const Engine::Components::Terrain& terrain) {
        const auto& vertices = mesh.GetVertices();
        const auto& indices = mesh.GetIndices();
        std::vector<uint32_t> edge(33, UINT32_MAX);
        for (uint32_t i = 0; i < vertices.size(); ++i)
            if (vertices[i].pos[0] == x)
            {
                const int y = static_cast<int>(vertices[i].pos[2]);
                if (y >= 0 && y <= 32)
                {
                    edge[y] = i;
                    Require(vertices[i].pos[1] == terrain.heights[
                        y * 33 + static_cast<int>(x)]);
                }
            }
        for (int y = 0; y < 32; ++y)
        {
            Require(edge[y] != UINT32_MAX && edge[y + 1] != UINT32_MAX);
            bool connected = false;
            for (size_t i = 0; i < indices.size(); i += 3)
                for (int side = 0; side < 3; ++side)
                    if ((indices[i + side] == edge[y] &&
                         indices[i + (side + 1) % 3] == edge[y + 1]) ||
                        (indices[i + side] == edge[y + 1] &&
                         indices[i + (side + 1) % 3] == edge[y]))
                        connected = true;
            Require(connected);
        }
    };
    checkBorder(*fine, 32.f, *lod->GetLoadedCell(1, 0));
    checkBorder(*coarse, 0.f, *lod->GetLoadedCell(2, 0));
    for (int y = 0; y <= 32; ++y)
        Require(lod->GetLoadedCell(1, 0)->heights[y * 33 + 32] ==
            lod->GetLoadedCell(2, 0)->heights[y * 33]);
    lod->SyncLoadedCells(2, 0);
    Require(lod->GetLoadedCell(2, 0)->GetMeshLodStride() == 1);
    Require(lod->GetLoadedCell(0, 0)->GetMeshLodStride() == 8);
    const size_t editedSample = 16 * 33 + 16;
    const float beforeEdit = lod->GetLoadedCell(2, 0)->heights[editedSample];
    Require(lod->ApplyBrush(80.f, 16.f, 1.f, 2.f,
        Engine::Components::Terrain::Brush::Raise));
    const float afterEdit = lod->GetLoadedCell(2, 0)->heights[editedSample];
    Require(afterEdit > beforeEdit);
    lod->SyncLoadedCells(0, 0);
    Require(lod->GetLoadedCell(2, 0)->GetMeshLodStride() == 8);
    Require(lod->GetLoadedCell(2, 0)->heights[editedSample] == afterEdit);
    lod->SyncLoadedCells(2, 0);
    Require(lod->GetLoadedCell(2, 0)->GetMeshLodStride() == 1);
    Require(lod->GetLoadedCell(2, 0)->heights[editedSample] == afterEdit);

    const auto cachePath = std::filesystem::temp_directory_path() /
        ("terrain_manager_cache_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto* cacheObject = scene.AddObject("Cached terrain");
    auto* cached = cacheObject->AddComponent<Engine::Components::TerrainManager>();
    cached->cellsX = cached->cellsY = 2;
    cached->loadedRadius = 0;
    cached->SetStaticCache(cachePath.string(), 77);
    int builds = 0;
    cached->SetProceduralBuilder([&builds](Engine::Components::TerrainManager::Cell,
        Engine::Components::Terrain& terrain) {
        ++builds;
        terrain.heights[4] = 4.f;
        terrain.Rebuild();
    });
    Require(cached->AddProceduralCell(0, 0));
    cached->SyncLoadedCells(0, 0);
    Require(builds == 1);
    Require(cached->GetLoadedCell(0, 0)->Owner->
        GetComponent<Engine::Components::Mesh>()->GetVertexCount() == 9);
    cached->SyncLoadedCells(10, 10);
    cached->SyncLoadedCells(0, 0);
    Require(builds == 1);
    Require(cached->GetLoadedCell(0, 0)->heights[4] == 4.f);
    cached->SetStaticCache(cachePath.string(), 78);
    Require(cached->GetLoadedCell(0, 0) == nullptr);
    cached->SyncLoadedCells(0, 0);
    Require(builds == 2);
    Require(cached->ApplyBrush(1.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    cached->SyncLoadedCells(10, 10);
    cached->SyncLoadedCells(0, 0);
    Require(builds == 2);
    Require(cached->GetLoadedCell(0, 0)->heights[4] == 5.f);
    std::filesystem::remove_all(cachePath);

    const auto editPath = std::filesystem::temp_directory_path() /
        ("terrain_world_edits_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto builder = [](Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain) {
        terrain.layers = {{"", -100.f, 100.f, {.3f, .4f, .2f}},
            {"", 100.f, 200.f, {.5f, .3f, .2f}}};
        std::fill(terrain.heights.begin(), terrain.heights.end(), cell.first * .1f);
        terrain.AssignLayersByHeight();
    };
    auto* worldObject = scene.AddObject("World manifest");
    auto* world = worldObject->AddComponent<Engine::Components::TerrainManager>();
    world->cellsX = world->cellsY = 2;
    world->loadedRadius = 0;
    world->cellDataDirectory = editPath.string();
    world->SetProceduralBuilder(builder);
    Require(world->AddProceduralCell(-64, 0));
    Require(world->AddProceduralCell(64, 0));
    world->SyncLoadedCells(-64, 0);
    Require(world->ApplyBrush(-127.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    world->SyncLoadedCells(64, 0);
    Require(world->ApplyBrush(129.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(world->ApplyBrush(129.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Paint, 1));
    const auto manifest = world->Serialize();
    Require(manifest["proceduralRuns"].ArraySize() == 2);
    Require(manifest["editedCells"].ArraySize() == 2);
    Require(world->GetStoredTerrainCount() == 0);
    const auto farFile = editPath / "-64_0.terrainedit";
    const auto farTime = std::filesystem::last_write_time(farFile);
    auto* restoredObject = scene.AddObject("Restored world");
    auto* restored = restoredObject->AddComponent<Engine::Components::TerrainManager>();
    restored->SetProceduralBuilder(builder);
    restored->Deserialize(manifest);
    restored->OnAfterDeserialize(nullptr);
    restored->SyncLoadedCells(-64, 0);
    Require(restored->GetLoadedCell(-64, 0)->heights[4] == -5.4f);
    restored->SyncLoadedCells(64, 0);
    Require(restored->GetLoadedCell(64, 0)->heights[4] == 7.4f);
    Require(restored->GetLoadedCell(64, 0)->weights[4 * 2 + 1] == 1.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(restored->ApplyBrush(129.f, 1.f, .5f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    restored->Serialize();
    Require(std::filesystem::last_write_time(farFile) == farTime);
    std::filesystem::remove_all(editPath);

    const auto seamPath = std::filesystem::temp_directory_path() /
        ("terrain_boundary_edits_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto seamBuilder = [](Engine::Components::TerrainManager::Cell cell,
        Engine::Components::Terrain& terrain) {
        terrain.layers = { {"", -1000.f, 1000.f, {.3f, .4f, .2f}},
            {"", -1000.f, 1000.f, {.5f, .3f, .2f}} };
        terrain.weights.assign(terrain.heights.size() * 2, 0.f);
        for (int row = 0; row <= terrain.cellsY; ++row)
            for (int col = 0; col <= terrain.cellsX; ++col)
            {
                const size_t index = static_cast<size_t>(
                    row * (terrain.cellsX + 1) + col);
                terrain.heights[index] = .01f * (cell.first * terrain.cellsX + col) +
                    .02f * (cell.second * terrain.cellsY + row);
                terrain.weights[index * 2] = 1.f;
            }
        terrain.Rebuild();
    };
    auto* seamObject = scene.AddObject("Boundary paint world");
    auto* seams = seamObject->AddComponent<Engine::Components::TerrainManager>();
    seams->cellsX = seams->cellsY = 4;
    seams->cellWidth = seams->cellDepth = 4.f;
    seams->loadedRadius = 0;
    seams->farRadius = -1;
    seams->lod2Radius = 1;
    seams->cellDataDirectory = seamPath.string();
    seams->SetProceduralBuilder(seamBuilder);
    for (const auto cell : {Engine::Components::TerrainManager::Cell{0, 0},
            {1, 0}, {0, 1}, {1, 1}, {-1024, 0}, {-1023, 0},
            {1023, 0}, {1024, 0}})
        Require(seams->AddProceduralCell(cell.first, cell.second));
    seams->SyncLoadedCells(0, 0);
    Require(seams->GetLoadedCellCount() == 1);
    Require(seams->ApplyBrush(16.f, 16.f, 5.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(seams->GetLoadedCellCount() == 4);
    Require(seams->ApplyBrush(16.f, 16.f, 5.f, 1.f,
        Engine::Components::Terrain::Brush::Average));
    Require(seams->ApplyBrush(16.f, 16.f, 5.f, 1.f,
        Engine::Components::Terrain::Brush::Paint, 1));
    const std::array<std::pair<Engine::Components::TerrainManager::Cell, size_t>, 4>
        corners{{{{0, 0}, 24}, {{1, 0}, 20}, {{0, 1}, 4}, {{1, 1}, 0}}};
    const float cornerHeight = seams->GetLoadedCell(0, 0)->heights[24];
    for (const auto& [cell, index] : corners)
    {
        auto* terrain = seams->GetLoadedCell(cell.first, cell.second);
        Require(terrain && terrain->heights[index] == cornerHeight);
        Require(terrain->weights[index * 2 + 1] == 1.f);
    }
    seams->farRadius = 2;
    seams->SyncLoadedCells(0, 0);
    Require(seams->GetLoadedCell(0, 0)->GetMeshLodStride() == 1);
    Require(seams->GetLoadedCell(1, 0)->GetMeshLodStride() == 2);
    const auto paintedMeshCorner = [](const Engine::Components::Terrain& terrain,
        float x, float z, float height) {
        const auto* mesh = terrain.Owner->GetComponent<Engine::Components::Mesh>();
        return mesh && std::any_of(mesh->GetVertices().begin(),
            mesh->GetVertices().end(), [&](const auto& vertex) {
                return vertex.pos[0] == x && vertex.pos[2] == z &&
                    vertex.pos[1] == height && vertex.color[0] == .5f &&
                    vertex.color[1] == .3f && vertex.color[2] == .2f;
            });
    };
    Require(paintedMeshCorner(*seams->GetLoadedCell(1, 0), 0.f, 16.f,
        cornerHeight));
    seams->SyncLoadedCells(1, 1);
    Require(seams->GetLoadedCell(1, 1)->GetMeshLodStride() == 1);
    Require(paintedMeshCorner(*seams->GetLoadedCell(1, 1), 0.f, 0.f,
        cornerHeight));
    seams->farRadius = -1;
    seams->SyncLoadedCells(-1024, 0);
    for (const auto& [cell, index] : corners)
        Require(std::filesystem::exists(seamPath /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) +
                ".terrainedit")));
    Require(seams->GetLoadedCell(-1023, 0) == nullptr);
    Require(!seams->ApplyBrush(-16368.0 + .0001, 8.0, .00005f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(seams->ApplyBrush(-16368.f, 8.f, 5.f, 1.f,
        Engine::Components::Terrain::Brush::Paint, 1));
    Require(seams->GetLoadedCell(-1023, 0) != nullptr);
    Require(seams->GetLoadedCell(-1024, 0)->weights[14 * 2 + 1] ==
        seams->GetLoadedCell(-1023, 0)->weights[10 * 2 + 1]);
    seams->SyncLoadedCells(1023, 0);
    Require(seams->GetLoadedCell(1024, 0) == nullptr);
    Require(!seams->ApplyBrush(16384.0 + .0001, 8.0, .00005f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(seams->ApplyBrush(16384.f, 8.f, 5.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(seams->GetLoadedCell(1024, 0) != nullptr);
    const float positiveHeight = seams->GetLoadedCell(1023, 0)->heights[14];
    Require(seams->GetLoadedCell(1024, 0)->heights[10] == positiveHeight);
    const auto seamManifest = seams->Serialize();
    Require(seamManifest["editedCells"].ArraySize() == 8);
    auto* seamRestoredObject = scene.AddObject("Restored boundary world");
    auto* seamRestored = seamRestoredObject->AddComponent<
        Engine::Components::TerrainManager>();
    seamRestored->SetProceduralBuilder(seamBuilder);
    seamRestored->Deserialize(seamManifest);
    seamRestored->OnAfterDeserialize(nullptr);
    seamRestored->SyncLoadedCells(0, 0);
    Require(seamRestored->GetLoadedCell(0, 0)->heights[24] == cornerHeight);
    Require(seamRestored->GetLoadedCell(0, 0)->weights[24 * 2 + 1] == 1.f);
    seamRestored->SyncLoadedCells(1, 1);
    Require(seamRestored->GetLoadedCell(1, 1)->heights[0] == cornerHeight);
    seamRestored->SyncLoadedCells(-1024, 0);
    Require(seamRestored->GetLoadedCell(-1024, 0)->weights[14 * 2 + 1] == 1.f);
    seamRestored->SyncLoadedCells(-1023, 0);
    Require(seamRestored->GetLoadedCell(-1023, 0)->weights[10 * 2 + 1] == 1.f);
    seamRestored->SyncLoadedCells(1023, 0);
    Require(seamRestored->GetLoadedCell(1023, 0)->heights[14] == positiveHeight);
    seamRestored->SyncLoadedCells(1024, 0);
    Require(seamRestored->GetLoadedCell(1024, 0)->heights[10] == positiveHeight);
    std::filesystem::remove_all(seamPath);

    // A sub-millimeter offset at either side of a 15.36 km route must survive
    // transforms, scene persistence, ray picking, and world-to-cell brushing.
    Engine::Scene::Scene precisionScene;
    auto* precisionCamera = precisionScene.AddObject("Precise camera");
    auto* camera = precisionCamera->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation = true;
    precisionCamera->transform.rotation.x = glm::half_pi<float>();
    for (double edge : {-7680.0, 7680.0})
    {
        precisionCamera->transform.position = {edge + .000123456, 20.0, 0.0};
        for (int step = 0; step < 100; ++step)
            precisionCamera->transform.position.x += .00001;
        const double expectedX = edge + .001123456;
        Require(std::abs(precisionCamera->transform.GetWorldPositionD().x -
            expectedX) < 1e-10);
        const glm::dmat4 inverse = glm::inverse(camera->GetProjectionMatrixD(1.0) *
            camera->GetViewMatrixD());
        glm::dvec4 nearPoint = inverse * glm::dvec4(0.0, 0.0, 0.0, 1.0);
        glm::dvec4 farPoint = inverse * glm::dvec4(0.0, 0.0, 1.0, 1.0);
        nearPoint /= nearPoint.w; farPoint /= farPoint.w;
        const double t = -nearPoint.y / (farPoint.y - nearPoint.y);
        const glm::dvec4 hit = nearPoint + (farPoint - nearPoint) * t;
        Require(std::abs(hit.x - expectedX) < 1e-7);
    }
    const double savedX = precisionCamera->transform.position.x;
    const std::string precisionXml = precisionScene.SaveToString();
    Engine::Scene::Scene reopenedPrecision;
    Require(Engine::Serialization::SceneSerializer::LoadFromString(
        reopenedPrecision, precisionXml, nullptr));
    Require(std::abs(reopenedPrecision.FindObjectByName("Precise camera")
        ->transform.position.x - savedX) < 1e-10);

    Engine::Scene::Scene collisionScene;
    for (double edge : {-7680.0, 7680.0})
    {
        auto* ground = collisionScene.AddObject("Edge ground");
        ground->transform.position = {edge, -.5, 0.0};
        auto* shape = ground->AddComponent<Engine::Components::PrimitiveObjectCollider>();
        shape->size = {4.f, 1.f, 4.f};
        ground->AddComponent<Engine::Components::RigidBody>()->bodyType = "Static";
    }
    collisionScene.Start();
    collisionScene.GetPhysics().Step(1.f / 60.f);
    for (double edge : {-7680.0, 7680.0})
    {
        const auto hits = collisionScene.GetPhysics().RaycastAll(
            {static_cast<float>(edge), 4.f, 0.f}, {0.f, -1.f, 0.f}, 10.f);
        Require(!hits.empty());
        Require(std::abs(hits.front().point.x - edge) < .01);
    }

    Engine::Scene::Scene runtimeTerrainScene;
    auto* runtimeMap = runtimeTerrainScene.AddObject("Static runtime map");
    auto* runtimeTerrain = runtimeMap->AddComponent<Engine::Components::TerrainManager>();
    runtimeTerrain->cellsX = runtimeTerrain->cellsY = 2;
    Require(runtimeTerrain->AddCell(0, 0));
    runtimeTerrainScene.Start();
    Require(!runtimeTerrain->ApplyBrush(1.0, 1.0, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(!runtimeTerrain->GetLoadedCell(0, 0)->ApplyBrush(
        1.f, 1.f, 1.f, 1.f, Engine::Components::Terrain::Brush::Raise));
    Require(!runtimeTerrain->AddCell(1, 0));
    Require(!runtimeTerrain->RemoveCell(0, 0));
    Require(runtimeTerrain->HasCell(0, 0));
    Require(!runtimeTerrain->HasCell(1, 0));
}
