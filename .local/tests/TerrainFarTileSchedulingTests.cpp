#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Object.h"
#include <stdexcept>
#include <utility>
#include <vector>
#include <iostream>
#include <chrono>
#include <thread>

int main()
{
    Engine::Scene::Scene scene;
    try {
    auto* owner = scene.AddObject("Far terrain scheduling test");
    auto* manager = owner->AddComponent<Engine::Components::TerrainManager>();
    const auto settle = [&](int x, int y) {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            manager->UpdateFarTerrain(x, y);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    manager->cellsX = 2;
    manager->cellsY = 2;
    manager->loadedRadius = 0;
    manager->farRadius = 2;
    manager->maxUploadsPerUpdate = 1;

    constexpr int extent = 32;
    const int side = extent * 2 + 2;
    std::vector<float> heights(static_cast<size_t>(side) * side, 0.f);
    Engine::Components::TerrainManager::FarTerrainPalette palette;
    for (auto& color : palette.farColors) color = {0.25f, 0.4f, 0.2f};
    for (auto& color : palette.nearColors) color = {0.25f, 0.4f, 0.2f};
    palette.archiveToNear = {0, 1, 2, 3};
    TerrainHeightArchive::FarPatch patch;
    patch.x = 3;
    patch.y = 3;
    patch.heights.resize(static_cast<size_t>(manager->cellsX + 1) *
        (manager->cellsY + 1), 1.f);
    manager->ConfigureFarTerrain(extent, std::move(heights),
        {std::move(patch)}, "", 1,
        std::move(palette));
    if (!manager->AddCell(6, 6))
        throw std::runtime_error("Could not create a distant resident cell");
    manager->UpdateStreaming(0, 0);
    if (!manager->GetLoadedCell(6, 6))
        throw std::runtime_error("Resident cell retired before tile coverage");

    manager->UpdateFarTerrain(0, 0);
    const uint64_t firstFrameUploads = manager->GetFarTileUploadCount();
    if (firstFrameUploads > 1)
        throw std::runtime_error("First far update exceeded its upload budget");
    settle(0, 0);
    manager->UpdateStreaming(0, 0);
    if (manager->GetLoadedCell(6, 6))
        throw std::runtime_error("Covered distant cell was not retired");
    const uint64_t firstUploads = manager->GetFarTileUploadCount();
    if (firstUploads < 2 || firstUploads <= firstFrameUploads)
        throw std::runtime_error("Initial far terrain was not revealed");

    uint64_t previous = firstUploads;
    for (int update = 0; update < 12; ++update)
    {
        manager->UpdateFarTerrain(1, 1);
        const uint64_t current = manager->GetFarTileUploadCount();
        if (current < previous || current - previous > 1)
            throw std::runtime_error("Far tile upload budget was exceeded");
        previous = current;
    }
    manager->UpdateFarTerrain(1, 1);
    if (manager->GetFarTileUploadCount() != previous)
        throw std::runtime_error("Unchanged far terrain was rebuilt");

    constexpr int largeExtent = 64;
    const int largeSide = largeExtent * 2 + 2;
    manager->SetStaticCache(".local/test-output/far_tile_cache_regression", 31);
    manager->ConfigureFarTerrain(largeExtent,
        std::vector<float>(static_cast<size_t>(largeSide) * largeSide, 0.f),
        {}, "", 2, {});
    settle(0, 0);
    if (manager->GetVisibleFarTileCount() >= 12 ||
        manager->GetVisibleFarTileVertexCount() >= 16000)
        throw std::runtime_error("Far tile hierarchy did not select coarse parents");
    manager->maxUploadsPerUpdate = 1;
    manager->maxFarRebuildMilliseconds = 4.f;
    for (int update = 0; update < 20; ++update)
    {
        manager->UpdateFarTerrain(40, 40);
        if (manager->GetVisibleFarTileCount() == 0)
            throw std::runtime_error("LOD transition hid all far terrain");
    }
    manager->ConfigureFarTerrain(largeExtent,
        std::vector<float>(static_cast<size_t>(largeSide) * largeSide, 0.f),
        {}, "", 2, {});
    settle(0, 0);
    const uint64_t warmHits = manager->GetFarTileCacheHits();
    if (warmHits == 0 ||
        manager->GetVisibleFarTileCount() >= 12)
        throw std::runtime_error("Versioned far tile cache was not reused");
    std::vector<float> editedHeights(
        static_cast<size_t>(largeSide) * largeSide, 0.f);
    editedHeights[static_cast<size_t>(40 + largeExtent) * largeSide +
        40 + largeExtent] = 4.f;
    manager->ConfigureFarTerrain(largeExtent, std::move(editedHeights),
        {}, "", 2, {});
    settle(0, 0);
    if (manager->GetFarTileCacheHits() >= warmHits)
        throw std::runtime_error("Edited far tile cache was not invalidated");
    Engine::Components::TerrainManager::FarTerrainPalette recolored;
    recolored.farColors[0] = {1.f, .25f, .25f};
    manager->ConfigureFarTerrain(largeExtent,
        std::vector<float>(static_cast<size_t>(largeSide) * largeSide, 0.f),
        {}, "", 2, recolored);
    settle(0, 0);
    if (manager->GetFarTileCacheHits() != 0)
        throw std::runtime_error("Appearance change reused stale far tiles");
    TerrainHeightArchive::FarPatch distantPatch;
    distantPatch.x = 40;
    distantPatch.y = 40;
    distantPatch.heights.resize(static_cast<size_t>(manager->cellsX + 1) *
        (manager->cellsY + 1), 2.f);
    auto configurePatched = [&] {
        manager->ConfigureFarTerrain(largeExtent,
            std::vector<float>(static_cast<size_t>(largeSide) * largeSide, 0.f),
            {distantPatch}, "", 2, {});
        settle(0, 0);
    };
    configurePatched();
    configurePatched();
    if (manager->GetFarTileCacheHits() < 2)
        throw std::runtime_error("Fine far tile resolution did not reuse cache");

    auto* cameraObject = scene.AddObject("Far tile test camera");
    cameraObject->transform.position = {0.f, 300.f, -300.f};
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation = false;
    camera->target = {0.f, 0.f, 0.f};
    camera->farPlane = 10000.f;
    std::vector<float> roughHeights(
        static_cast<size_t>(largeSide) * largeSide, 0.f);
    for (int y = 1; y < largeSide; y += 2)
        for (int x = 1; x < largeSide; x += 2)
            roughHeights[static_cast<size_t>(y) * largeSide + x] = 10.f;
    manager->ConfigureFarTerrain(largeExtent, roughHeights,
        {}, "", 9, {});
    manager->maxUploadsPerUpdate = 64;
    manager->farTileScreenErrorPixels = 64.f;
    for (int update = 0; update < 8; ++update)
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
    for (int update = 0; update < 50; ++update)
    {
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const size_t coarseCount = manager->GetVisibleFarTileCount();
    if (coarseCount < 2)
        throw std::runtime_error("Far tile scheduling starved distant regions");
    camera->farPlane = 1.f;
    manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
    if (manager->GetVisibleFarTileCount() != 0)
        throw std::runtime_error("Camera view distance did not cull far tiles");
    camera->farPlane = 10000.f;
    for (int update = 0; update < 50; ++update)
    {
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    manager->farTileScreenErrorPixels = .1f;
    for (int update = 0; update < 8; ++update)
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
    for (int update = 0; update < 50; ++update)
    {
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (manager->GetVisibleFarTileCount() <= coarseCount)
        throw std::runtime_error("Projected error did not refine visible far tiles");

    // The hierarchy must keep growing on maps wider than the original
    // 129-cell benchmark, without instantiating one object per map cell.
    constexpr int extendedExtent = 192;
    const int extendedSide = extendedExtent * 2 + 2;
    manager->ConfigureFarTerrain(extendedExtent,
        std::vector<float>(static_cast<size_t>(extendedSide) *
            extendedSide, 0.f), {}, "", 12, {});
    manager->farTileScreenErrorPixels = 64.f;
    camera->farPlane = 100000.f;
    for (int update = 0; update < 100; ++update)
    {
        manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (manager->GetFarTileNodeCount() < 100 ||
        manager->GetVisibleFarTileCountForStride(8) == 0 ||
        manager->GetVisibleFarTileCount() >= 100)
        throw std::runtime_error("Extended map did not use coarse hierarchy");
    const size_t extendedVisible = manager->GetVisibleFarTileCount();
    manager->maxFarViewDistanceMeters = 500.f;
    manager->UpdateFarTerrain(0, 0, camera, 1080, 16.f / 9.f);
    if (manager->GetVisibleFarTileCount() >= extendedVisible)
        throw std::runtime_error("Independent far distance cap had no effect");
    manager->maxFarViewDistanceMeters = 0.f;

    Engine::Components::TerrainManager::FarTerrainPalette brushPalette;
    brushPalette.nearColors[1] = {1.f, 0.f, 0.f};
    manager->ConfigureFarTerrain(extent,
        std::vector<float>(static_cast<size_t>(side) * side, 0.f),
        {}, "", 4, brushPalette);
    manager->UpdateFarTerrain(0, 0);
    const uint64_t beforeBrush = manager->GetFarTileUploadCount();
    if (!manager->AddCell(3, 3))
        throw std::runtime_error("Could not add editable terrain cell");
    auto* editable = manager->GetLoadedCell(3, 3);
    if (!editable)
        throw std::runtime_error("Editable terrain cell was not loaded");
    editable->layers.resize(6);
    editable->weights.assign(editable->heights.size() * 6, 0.f);
    for (size_t i = 0; i < editable->heights.size(); ++i)
        editable->weights[i * 6] = 1.f;
    if (!manager->ApplyBrush(7.0, 7.0, .6f, .5f,
            Engine::Components::Terrain::Brush::Raise, 0) ||
        !manager->ApplyBrush(7.0, 7.0, .6f, .5f,
            Engine::Components::Terrain::Brush::Paint, 1))
        throw std::runtime_error("Editor terrain brush did not change the cell");
    manager->editorRadius = 2;
    manager->UpdateEditorStreaming({0.0, 0.0, 0.0});
    settle(0, 0);
    if (manager->GetFarTileUploadCount() <= beforeBrush)
        throw std::runtime_error("Live brush did not rebuild a far tile");
    auto* paintedTile = scene.FindObjectByName("Distant terrain tile 0,0");
    if (!paintedTile || !paintedTile->GetComponent<Engine::Components::Mesh>())
        throw std::runtime_error("Painted distant tile was not created");
    bool hasPaintColor = false;
    for (const auto& vertex : paintedTile->GetComponent<
        Engine::Components::Mesh>()->GetVertices())
        hasPaintColor |= vertex.color[0] > .1f;
    if (!hasPaintColor)
        throw std::runtime_error("Live paint was not applied to far geometry");
    settle(0, 0);
    const uint64_t beforePartialUpload = manager->GetFarTileUploadedBytes();
    const size_t beforePartialVertices = paintedTile->GetComponent<
        Engine::Components::Mesh>()->GetVertices().size();
    if (!manager->ApplyBrush(7.0, 7.0, .6f, .25f,
            Engine::Components::Terrain::Brush::Raise, 0))
        throw std::runtime_error("Repeated brush edit did not change terrain");
    manager->UpdateEditorStreaming({0.0, 0.0, 0.0});
    const uint64_t partialBytes = manager->GetFarTileUploadedBytes() -
        beforePartialUpload;
    const size_t patchBytes = static_cast<size_t>(manager->cellsX + 1) *
        (manager->cellsY + 1) * sizeof(Engine::Components::Mesh::Vertex);
    if (partialBytes == 0 || partialBytes >
        patchBytes ||
        paintedTile->GetComponent<Engine::Components::Mesh>()->GetVertices().size()
            != beforePartialVertices)
        throw std::runtime_error("Repeated brush edit rebuilt the whole far tile");

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
