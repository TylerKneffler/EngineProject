#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <chrono>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <stdexcept>

static void Require(bool condition)
{
    if (!condition) throw std::runtime_error("Terrain manager regression failed");
}

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
    std::filesystem::remove_all(asyncCachePath);
    std::filesystem::remove_all(asyncEditPath);

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
}
