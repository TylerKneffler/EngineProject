#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Window.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

static void Require(bool condition)
{
    if (!condition) throw std::runtime_error("Terrain performance scene regression failed");
}

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    const bool benchmark = std::getenv("TERRAIN_PERFORMANCE_BENCHMARK") != nullptr;
    const unsigned width = benchmark ? 960u : 64u;
    const unsigned height = benchmark ? 540u : 64u;
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Terrain performance scene test", width, height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), width, height)) return 0;
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    Require(scene.Load("Engine/Core/Assets/Scenes/terrain/terrain_performance.scene"));
    auto* object = scene.FindObjectByName("Terrain Performance Map");
    Require(object != nullptr);
    auto* manager = object->GetComponent<Engine::Components::TerrainManager>();
    auto* driver = object->GetComponent<TerrainPerformanceDriver>();
    auto* noise = object->GetComponent<PerlinNoiseField>();
    Require(manager && driver && noise);
    Require(manager->cellsX == 32 && manager->cellWidth == 4.f);
    Require(manager->loadedRadius == 2 && manager->farRadius == 32);
    Require(manager->asyncStreaming && manager->maxUploadsPerUpdate == 1);
    Require(driver->mapHalfExtentCells == 64);
    Require(driver->cacheDirectory == ".local/cache/terrain_performance");
    Require(manager->cellDataDirectory ==
        "Engine/Core/Assets/TerrainData/TerrainPerformance");
    // The short benchmark and edit checks use a small resident set. The scene
    // itself exercises the several-kilometer far radius during normal play.
    manager->farRadius = -1;
    driver->cellsPerUpdate = 4;
    driver->generationRadiusCells = 1;
    auto* camera = scene.FindObjectByName("Terrain Performance Camera");
    Require(camera != nullptr);
    camera->transform.position = {0.f, 110.f, 0.f};
    if (benchmark)
    {
        driver->generationRadiusCells = 8;
        driver->cellsPerUpdate = 32;
        for (int i = 0; i < 16; ++i) driver->Update();
        auto* cameraComponent = camera->GetComponent<Engine::Components::Camera>();
        if (!std::getenv("TERRAIN_STREAMING_ONLY"))
        for (int radius : {1, 2, 3, 4, 6, 8})
        {
            manager->loadedRadius = radius;
            manager->SyncLoadedCells(0, 0);
            double elapsedMs = 0.0;
            for (int frame = 0; frame < 40; ++frame)
            {
                const auto start = std::chrono::steady_clock::now();
                renderer.BeginFrame();
                renderer.Clear(.2f, .25f, .3f);
                auto context = renderer.CreateFrameGraphicsContext();
                scene.PrepareRenderFrame();
                scene.Render(context.get(), 1.f, cameraComponent, false, width, height);
                renderer.EndFrame();
                renderer.WaitIdle();
                if (frame >= 10)
                    elapsedMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
            }
            std::cout << "radius=" << radius << " distance=" << radius * 128
                << "m loaded=" << manager->GetLoadedCellCount()
                << " frameMs=" << elapsedMs / 30.0
                << " fps=" << 30000.0 / elapsedMs << '\n';
        }
        manager->loadedRadius = 2;
        driver->generationRadiusCells = 2;
        driver->cellsPerUpdate = 2;
        camera->transform.position = {2048.f, 110.f, 0.f};
        double streamingMs = 0.0, worstMs = 0.0, movingMs = 0.0, movingWorstMs = 0.0;
        for (int frame = 0; frame < 30; ++frame)
        {
            const auto start = std::chrono::steady_clock::now();
            driver->Update();
            manager->UpdateStreaming(16, 0);
            renderer.BeginFrame();
            renderer.Clear(.2f, .25f, .3f);
            auto context = renderer.CreateFrameGraphicsContext();
            scene.PrepareRenderFrame();
            scene.Render(context.get(), 1.f, cameraComponent, false, width, height);
            renderer.EndFrame();
            renderer.WaitIdle();
            const double frameMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            streamingMs += frameMs;
            worstMs = std::max(worstMs, frameMs);
            if (frame > 0)
            {
                movingMs += frameMs;
                movingWorstMs = std::max(movingWorstMs, frameMs);
            }
        }
        std::cout << "streaming radius=2 avgMs=" << streamingMs / 30.0
            << " worstMs=" << worstMs << " afterJumpAvgMs=" << movingMs / 29.0
            << " afterJumpWorstMs=" << movingWorstMs << '\n';
        return 0;
    }
    for (int i = 0; i < 3; ++i) driver->Update();
    Require(manager->BakeProceduralCell(0, 0, true));
    Require(std::filesystem::file_size(manager->GetCacheFilePath(0, 0)) < 60000);
    const size_t cacheHitsBeforeLoad = manager->GetCacheHitCount();
    for (int attempt = 0; attempt < 500 && !manager->GetLoadedCell(0, 0); ++attempt)
    {
        manager->UpdateStreaming(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(manager->GetLoadedCell(0, 0) != nullptr);
    Require(manager->GetCacheHitCount() > cacheHitsBeforeLoad);
    manager->SyncLoadedCells(0, 0);
    Require(std::filesystem::exists(driver->cacheDirectory) &&
        !std::filesystem::is_empty(driver->cacheDirectory));
    Require(manager->GetCellCount() >= 2);
    Require(manager->GetStoredTerrainCount() == 0);
    auto* center = manager->GetLoadedCell(0, 0);
    auto* neighbor = manager->GetLoadedCell(1, 0);
    Require(center && neighbor);
    Require(center->layers.size() == 4);
    Require(center->GetLayerMaterials().size() == 4);
    for (size_t layer = 0; layer < 4; ++layer)
    {
        const auto& material = center->GetLayerMaterials()[layer];
        Require(material && material->baseColorTexture &&
            material->baseColorTexture->GetGraphicsTexture());
    }
    for (size_t vertex = 0; vertex < center->heights.size(); ++vertex)
    {
        const float sampleHeight = center->heights[vertex];
        const size_t expected = sampleHeight < -12.f ? 0 : sampleHeight < 0.f ? 1 :
            sampleHeight < 16.f ? 2 : 3;
        Require(center->weights[vertex * 4 + expected] == 1.f);
        const auto* mesh = center->Owner->GetComponent<Engine::Components::Mesh>();
        Require(mesh && mesh->GetVertices()[vertex].uvChannels[2][expected] == 1.f);
    }
    for (int row = 0; row <= manager->cellsY; ++row)
    {
        const float a = center->heights[static_cast<size_t>(row * 33 + 32)];
        const float b = neighbor->heights[static_cast<size_t>(row * 33)];
        Require(std::abs(a - b) < .0001f);
    }
    manager->loadedRadius = 0;
    manager->farRadius = 2;
    manager->lod2Radius = 1;
    manager->SyncLoadedCells(0, 0);
    Require(center->GetMeshLodStride() == 1);
    Require(neighbor->GetMeshLodStride() == 2);
    Require(neighbor->Owner->GetComponent<Engine::Components::Mesh>()->
        GetVertexCount() < center->Owner->GetComponent<Engine::Components::Mesh>()->
        GetVertexCount());
    renderer.BeginFrame();
    renderer.Clear(.2f, .25f, .3f);
    auto lodContext = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(lodContext.get(), 1.f,
        camera->GetComponent<Engine::Components::Camera>(), false, width, height);
    renderer.EndFrame();
    renderer.WaitIdle();
    manager->loadedRadius = 2;
    manager->farRadius = -1;
    manager->SyncLoadedCells(0, 0);
    const float originalHeight = center->heights[34];
    const auto editPath = std::filesystem::temp_directory_path() /
        ("terrain_performance_edits_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    manager->cellDataDirectory = editPath.string();
    Require(manager->ApplyBrush(4.f, 4.f, 1.f, 1.f,
        Engine::Components::Terrain::Brush::Raise));
    Require(manager->GetStoredTerrainCount() == 0);
    const auto saved = manager->Serialize();
    Require(saved["editedCells"].ArraySize() == 1);
    Require(std::filesystem::exists(std::filesystem::path(manager->cellDataDirectory) /
        "0_0.terrainedit"));
    manager->Deserialize(saved);
    driver->Update();
    manager->SyncLoadedCells(0, 0);
    Require(manager->GetLoadedCell(0, 0)->heights[34] == originalHeight + 1.f);
    manager->loadedRadius = 0;
    manager->SyncLoadedCells(0, 0);
    Require(manager->GetLoadedCellCount() <= 1);
    std::filesystem::remove_all(editPath);
}
