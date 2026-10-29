#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Scripts/TerrainPerformance/TerrainPerformanceDriver.h"
#include "Scripts/TerrainPerformance/TerrainHeightArchive.h"
#include "Core/Window.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void RequireAt(bool condition, int line)
{
    if (!condition)
    {
        std::cerr << "Terrain performance scene regression failed at line "
            << line << '\n';
        throw std::runtime_error(
        "Terrain performance scene regression failed at line " +
        std::to_string(line));
    }
}
#define Require(condition) RequireAt((condition), __LINE__)

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    const bool benchmark = std::getenv("TERRAIN_PERFORMANCE_BENCHMARK") != nullptr;
    const bool benchmarkShadows = std::getenv("TERRAIN_SHADOW_BENCHMARK") != nullptr;
    const bool captureDummy = std::getenv("TERRAIN_DUMMY_CAPTURE") != nullptr;
    const bool captureGrid = std::getenv("TERRAIN_GRID_CAPTURE") != nullptr;
    const unsigned width = captureDummy || captureGrid ? 1280u : (benchmark ? 960u : 64u);
    const unsigned height = captureDummy || captureGrid ? 720u : (benchmark ? 540u : 64u);
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Terrain performance scene test", width, height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), width, height)) return 0;
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    Require(scene.Load("Engine/Core/Assets/Scenes/terrain/terrain_performance.scene"));
    if (std::getenv("TERRAIN_STARTUP_WAIT_TEST"))
    {
        auto* viewer = scene.FindObjectByName("Terrain Performance Capsule");
        auto* map = scene.FindObjectByName("Terrain Performance Map");
        auto* mapDriver = map ? map->GetComponent<TerrainPerformanceDriver>() : nullptr;
        auto* mapManager = map ? map->GetComponent<Engine::Components::TerrainManager>() : nullptr;
        Require(viewer && mapDriver && mapManager);
        scene.Start();
        const double startX = viewer->transform.GetWorldPositionD().x;
        bool dataReady = false, moved = false;
        for (int frame = 0; frame < 1200 && !moved; ++frame)
        {
            scene.Update(1.f / 60.f);
            const bool ready = mapDriver->IsInitialMapDataReady();
            const double currentX = viewer->transform.GetWorldPositionD().x;
            if (!ready && std::abs(currentX - startX) > 0.01)
                throw std::runtime_error("Camera moved before map data was ready");
            dataReady |= ready;
            moved = std::abs(currentX - startX) > 0.01;
            if (moved)
                Require(ready && mapManager->GetLoadedCellCount() >= 49u);
            if (frame % 120 == 0)
                std::cout << "frame=" << frame << " data=" << ready
                    << " near=" << mapManager->GetLoadedCellCount()
                    << " far=" << mapManager->GetBuiltFarTileCount()
                    << '/' << mapManager->GetDesiredFarTileCount()
                    << " x=" << currentX << '\n';
        }
        Require(dataReady && moved);
        std::cout << "Terrain movement waited for map data and 49 near cells\n";
        return 0;
    }
    auto* sunObject = scene.FindObjectByName("Terrain Performance Sun");
    Require(sunObject != nullptr);
    auto* sun = sunObject->GetComponent<Engine::Components::Light>();
    Require(sun && sun->castsShadows && sun->shadowResolutionScale == 0.5f);
    Require(std::abs(sunObject->transform.rotation.x - 1.3962634f) < 0.001f);
    auto* object = scene.FindObjectByName("Terrain Performance Map");
    Require(object != nullptr);
    auto* manager = object->GetComponent<Engine::Components::TerrainManager>();
    auto* driver = object->GetComponent<TerrainPerformanceDriver>();
    Require(manager && driver && manager->requireRuntimeBake);
    auto* scaleDummy = scene.FindObjectByName("Terrain Scale Dummy (1.8 m)");
    Require(scaleDummy != nullptr);
    Require(scaleDummy->transform.position.x == 0.0 &&
        scaleDummy->transform.position.z == 0.0);
    Require(std::abs(scaleDummy->transform.scale.y - 0.01f) < 0.0001f);
    auto* dummyMesh = scaleDummy->GetComponent<Engine::Components::Mesh>();
    Require(dummyMesh && dummyMesh->IsReady() &&
        dummyMesh->GetVertexCount() > 20000);
    float foot = 1000.f, head = -1000.f;
    for (const auto& vertex : dummyMesh->GetVertices())
    {
        foot = std::min(foot, vertex.pos[2]);
        head = std::max(head, vertex.pos[2]);
    }
    Require(std::abs((head - foot) * scaleDummy->transform.scale.y -
        1.81621f) < 0.01f);
    std::vector<float> originHeights;
    Require(TerrainHeightArchive::ReadCell(
        "Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights",
        driver->SourceSignature(), 64, 32, 32, 0, 0, originHeights));
    Require(std::abs(scaleDummy->transform.position.y +
        static_cast<double>(foot) * 0.01 -
        static_cast<double>(originHeights.front())) < 0.02);
    if (captureDummy || captureGrid)
    {
        manager->SyncLoadedCells(0, -1);
        auto* viewObject = scene.FindObjectByName("Terrain Performance Camera");
        auto* viewCamera = viewObject->GetComponent<Engine::Components::Camera>();
        if (std::getenv("TERRAIN_DUMMY_CAPTURE_NO_SHADOWS") != nullptr)
            sun->castsShadows = false;
        viewObject->transform.position = captureGrid
            ? glm::dvec3{32.0, 90.0, -100.0}
            : glm::dvec3{0.0, -15.85, -5.0};
        viewObject->transform.rotation = captureGrid
            ? glm::vec3{0.65f, 0.f, 0.f}
            : glm::vec3{0.04f, 0.f, 0.f};
        manager->measurementGrid = captureGrid;
        Require(renderer.EnableOffscreenExport(width, height, 1, 2));
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.2f, 0.25f, 0.3f);
        auto context = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(context.get(), 1.f, viewCamera, false,
            width, height, true);
        scene.Render(context.get(), 1.f, viewCamera, false, width, height);
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> pixels;
        Require(renderer.ReadExportFrameRGBA(pixels, true));
        const size_t groundPixel =
            (static_cast<size_t>(height * 3 / 4) * width + width / 2) * 4;
        Require(groundPixel + 2 < pixels.size() &&
            (captureGrid || pixels[groundPixel] + pixels[groundPixel + 1] +
                pixels[groundPixel + 2] > 150));
        const auto image = std::filesystem::path(
            captureGrid
                ? ".local/performance-logs/terrain-captures/measurement_grid.rgba"
                : ".local/performance-logs/terrain-captures/scale_dummy.rgba");
        std::filesystem::create_directories(image.parent_path());
        std::ofstream output(image, std::ios::binary);
        output.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
        Require(output.good());
        std::cout << "Captured " << image << '\n';
        return 0;
    }
    Require(manager->cellsX == 32 && manager->cellWidth == 4.f);
    Require(manager->loadedRadius == 2 && manager->farRadius == 16);
    Require(manager->fastRuntimeLighting);
    Require(manager->asyncStreaming && manager->maxUploadsPerUpdate == 1);
    Require(manager->GetCellCount() == 16641);
    Require(manager->GetStoredTerrainCount() == 0);
    Require(manager->GetLoadedCellCount() > 0);
    Require(manager->GetLoadedCell(0, -2) != nullptr);
    Require(manager->GetLoadedCell(0, -2)->textureTiling == 32.f);
    const auto manifest = manager->Serialize();
    Require(manifest["proceduralRuns"].ArraySize() == 129);
    Require(manifest["cells"].ArraySize() == 0);
    const std::string savedScene = scene.SaveToString();
    Require(savedScene.size() < 200000);
    Require(savedScene.find("TerrainManager.proceduralRuns") != std::string::npos);
    Require(scene.editorCamera.transform.position == glm::dvec3(0.0, 180.0, -300.0));
    Require(scene.editorCamera.GetComponent<Engine::Components::Camera>()->farPlane == 1000.f);
    Require(manager->editorRadius == 6);
    Require(scene.FindObjectByName("Terrain Performance Camera")
        ->GetComponent<Engine::Components::Camera>()->farPlane == 25000.f);
    if (!benchmark)
    {
        Require(renderer.EnableOffscreenExport(width, height, 1, 2));
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto previewContext = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(previewContext.get(), 1.f, nullptr, false, width, height);
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> previewPixels;
        Require(renderer.ReadExportFrameRGBA(previewPixels, true));
        size_t terrainPixels = 0;
        for (size_t i = 0; i + 3 < previewPixels.size(); i += 4)
            terrainPixels += previewPixels[i] + previewPixels[i + 1] +
                previewPixels[i + 2] > 30;
        Require(terrainPixels > 64);
        manager->measurementGrid = true;
        Require(manager->Serialize()["measurementGrid"].AsBool());
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto gridContext = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(gridContext.get(), 1.f, nullptr, false, width, height);
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> gridPixels;
        Require(renderer.ReadExportFrameRGBA(gridPixels, true));
        Require(gridPixels.size() == previewPixels.size());
        size_t changedPixels = 0;
        for (size_t i = 0; i + 3 < gridPixels.size(); i += 4)
            changedPixels += std::abs(static_cast<int>(gridPixels[i]) -
                static_cast<int>(previewPixels[i])) > 20;
        Require(changedPixels > 64);
        manager->measurementGrid = false;
    }
    Require(driver->mapHalfExtentCells == 64);
    Require(driver->cacheDirectory == ".local/cache/terrain_performance");
    Require(driver->heightArchive ==
        "Assets/TerrainData/TerrainPerformance/world.heights");
    std::vector<float> farHeights;
    Require(TerrainHeightArchive::ReadFarGrid(
        "Engine/Core/Assets/TerrainData/TerrainPerformance/world.fargrid",
        driver->SourceSignature(), 64, farHeights));
    Require(farHeights.size() == 130u * 130u);
    const auto patchDirectory = std::filesystem::temp_directory_path() /
        ("terrain_far_patch_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(patchDirectory);
    const auto patchFile = patchDirectory / "60_60.terrainedit";
    {
        std::ofstream file(patchFile, std::ios::binary);
        const uint64_t magic = 0x31544944454e5254ull;
        const uint32_t version = 1, side = 32, layerCount = 4;
        const uint32_t heightCount = 1, weightCount = 0;
        const uint32_t center = 16u * 33u + 16u;
        const float sculptedHeight = 100.f;
        Require(TerrainHeightArchive::Write(file, magic) &&
            TerrainHeightArchive::Write(file, version) &&
            TerrainHeightArchive::Write(file, side) &&
            TerrainHeightArchive::Write(file, side) &&
            TerrainHeightArchive::Write(file, layerCount) &&
            TerrainHeightArchive::Write(file, heightCount) &&
            TerrainHeightArchive::Write(file, weightCount) &&
            TerrainHeightArchive::Write(file, center) &&
            TerrainHeightArchive::Write(file, sculptedHeight));
    }
    const auto patchedGrid = patchDirectory / "patched.fargrid";
    Require(TerrainHeightArchive::BakeFarGrid(
        "Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights",
        patchedGrid.string(), driver->SourceSignature(), 64, 32, 32,
        patchDirectory.string(), {{60, 60}}));
    std::vector<TerrainHeightArchive::FarPatch> farPatches;
    Require(TerrainHeightArchive::ReadFarGrid(patchedGrid.string(),
        driver->SourceSignature(), 64, farHeights, &farPatches));
    Require(farPatches.size() == 1 && farPatches[0].x == 60 &&
        farPatches[0].y == 60 && farPatches[0].heights[16 * 33 + 16] == 100.f);
    for (int edge : {-64, 64})
    {
        const int cell = edge == -64 ? -64 : 64;
        std::vector<float> cornerHeights;
        Require(TerrainHeightArchive::ReadCell(
            "Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights",
            driver->SourceSignature(), 64, 32, 32, cell, cell,
            cornerHeights));
        const size_t gridIndex = edge == -64 ? 0 : farHeights.size() - 1;
        const size_t cellIndex = edge == -64 ? 0 : cornerHeights.size() - 1;
        Require(farHeights[gridIndex] == cornerHeights[cellIndex]);
    }
    Require(std::filesystem::file_size(
        "Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights") < 42000000);
    const std::string savedHeights =
        "Engine/Core/Assets/TerrainData/TerrainPerformance/world.heights";
    for (int edge : {-64, 63})
    {
        std::vector<float> leftHeights, rightHeights;
        Require(TerrainHeightArchive::ReadCell(savedHeights,
            driver->SourceSignature(), 64, 32, 32, edge, 0, leftHeights));
        Require(TerrainHeightArchive::ReadCell(savedHeights,
            driver->SourceSignature(), 64, 32, 32, edge + 1, 0, rightHeights));
        for (int row = 0; row <= 32; ++row)
            Require(leftHeights[row * 33 + 32] == rightHeights[row * 33]);
    }
    Require(manager->GetLoadedCell(0, -2)->Owner->
        GetComponent<Engine::Components::Mesh>()->IsReady());
    Require(manager->cellDataDirectory ==
        "Engine/Core/Assets/TerrainData/TerrainPerformance");
    // The short benchmark and edit checks use a small resident set. The scene
    // itself exercises the several-kilometer far radius during normal play.
    manager->farRadius = -1;
    auto* camera = scene.FindObjectByName("Terrain Performance Camera");
    Require(camera != nullptr);
    camera->transform.position = {0.f, 110.f, 0.f};
    if (std::getenv("TERRAIN_MEMORY_BENCHMARK"))
    {
        manager->loadedRadius = 2;
        manager->farRadius = 8;
        manager->lod2Radius = 2;
        manager->lod4Radius = 4;
        manager->maxLoadsPerUpdate = 8;
        manager->maxUnloadsPerUpdate = 32;
        manager->maxPreparedCells = 12;
        manager->maxUploadsPerUpdate = 4;
        struct Peak { uint64_t cpu = 0, gpu = 0, cache = 0, working = 0, privateBytes = 0; size_t cells = 0; };
        const auto sample = [&]() {
            Peak result;
            const auto usage = manager->GetMemoryUsage();
            result.cpu = usage.cpuBytes;
            result.gpu = usage.gpuBytes;
            result.cache = usage.cacheBytes;
            result.cells = manager->GetLoadedCellCount();
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            {
                result.working = counters.WorkingSetSize;
                result.privateBytes = counters.PrivateUsage;
            }
            return result;
        };
        const auto measure = [&](const char* phase, const std::vector<int>& positions) {
            Peak peak;
            for (size_t position = 0; position < positions.size(); ++position)
                for (int frame = 0; frame <
                    (position + 1 == positions.size() ? 360 : 12); ++frame)
                {
                    const int x = positions[position];
                    camera->transform.position.x = x * 128.f + 64.f;
                    manager->UpdateStreaming(x, 0);
                    const Peak current = sample();
                    peak.cpu = std::max(peak.cpu, current.cpu);
                    peak.gpu = std::max(peak.gpu, current.gpu);
                    peak.cache = std::max(peak.cache, current.cache);
                    peak.working = std::max(peak.working, current.working);
                    peak.privateBytes = std::max(peak.privateBytes, current.privateBytes);
                    peak.cells = std::max(peak.cells, current.cells);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            const double mib = 1024.0 * 1024.0;
            std::cout << "memory phase=" << phase
                << " cells=" << peak.cells
                << " cpuMiB=" << peak.cpu / mib
                << " gpuMiB=" << peak.gpu / mib
                << " cacheMiB=" << peak.cache / mib
                << " workingMiB=" << peak.working / mib
                << " privateMiB=" << peak.privateBytes / mib << '\n';
            Require(peak.cpu <= static_cast<uint64_t>(manager->cpuBudgetMiB) * 1024 * 1024);
            Require(peak.gpu <= static_cast<uint64_t>(manager->gpuBudgetMiB) * 1024 * 1024);
            Require(peak.cache <= static_cast<uint64_t>(manager->cacheBudgetMiB) * 1024 * 1024);
            return peak;
        };
        std::vector<int> forward, backward;
        for (int x = -60; x <= 60; x += 4) forward.push_back(x);
        for (int x = 60; x >= -60; x -= 4) backward.push_back(x);
        measure("traverse", forward);
        measure("teleport", {-60, 60});
        measure("backtrack", backward);
        return 0;
    }
    if (benchmark)
    {
        for (int i = 0; i < 16; ++i) driver->Update();
        auto* cameraComponent = camera->GetComponent<Engine::Components::Camera>();
        if (std::getenv("TERRAIN_BATCH_BENCHMARK"))
        {
            manager->loadedRadius = 2;
            manager->farRadius = 8;
            manager->lod2Radius = 2;
            manager->batchFarCells = !std::getenv("TERRAIN_DISABLE_BATCHING");
            manager->SyncLoadedCells(0, 0);
            double frameMs = 0.0, renderMs = 0.0, terrainGpuMs = 0.0;
            uint32_t gpuSamples = 0, draws = 0;
            for (int frame = 0; frame < 40; ++frame)
            {
                const auto frameStart = std::chrono::steady_clock::now();
                renderer.BeginFrame();
                renderer.Clear(.2f, .25f, .3f);
                auto context = renderer.CreateFrameGraphicsContext();
                scene.PrepareRenderFrame();
                const auto renderStart = std::chrono::steady_clock::now();
                scene.Render(context.get(), 1.f, cameraComponent, false, width, height);
                const double renderTime = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - renderStart).count();
                draws = scene.GetLastOrdinaryDrawCount();
                renderer.EndFrame();
                renderer.WaitIdle();
                if (frame < 10) continue;
                frameMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - frameStart).count();
                renderMs += renderTime;
                const auto telemetry = renderer.GetFrameTimingTelemetry();
                if (telemetry.gpuTimingsValid)
                {
                    terrainGpuMs += telemetry.gpuMilliseconds[static_cast<size_t>(
                        Engine::Graphics::GpuTimingStage::Terrain)];
                    ++gpuSamples;
                }
            }
            std::cout << "batch=" << manager->batchFarCells
                << " loaded=" << manager->GetLoadedCellCount()
                << " batches=" << manager->GetRenderBatchCount()
                << " draws=" << draws << " frameMs=" << frameMs / 30.0
                << " renderCpuMs=" << renderMs / 30.0
                << " terrainGpuMs=" << (gpuSamples ? terrainGpuMs / gpuSamples : 0.0)
                << " gpuSamples=" << gpuSamples << '\n';
            return 0;
        }
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
        camera->transform.position = {2048.f, 110.f, 0.f};
        double streamingMs = 0.0, worstMs = 0.0, movingMs = 0.0, movingWorstMs = 0.0;
        double renderCpuMs = 0.0, terrainGpuMs = 0.0, shadowGpuMs = 0.0;
        uint32_t gpuSamples = 0, ordinaryDraws = 0, shadowDraws = 0;
        uint32_t shadowCandidates = 0;
        for (int frame = 0; frame < 30; ++frame)
        {
            const auto start = std::chrono::steady_clock::now();
            driver->Update();
            manager->UpdateStreaming(16, 0);
            renderer.BeginFrame();
            renderer.Clear(.2f, .25f, .3f);
            auto context = renderer.CreateFrameGraphicsContext();
            scene.PrepareRenderFrame();
            const auto renderStart = std::chrono::steady_clock::now();
            if (benchmarkShadows)
            {
                scene.Render(context.get(), 1.f, cameraComponent, false,
                    width, height, true);
                const auto& shadow = scene.GetShadowDebugSnapshot();
                shadowDraws = shadow.submittedCasterDrawCount;
                shadowCandidates = shadow.casterCount * shadow.cascadeCount;
            }
            scene.Render(context.get(), 1.f, cameraComponent, false, width, height);
            renderCpuMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - renderStart).count();
            ordinaryDraws = scene.GetLastOrdinaryDrawCount();
            renderer.EndFrame();
            renderer.WaitIdle();
            const auto telemetry = renderer.GetFrameTimingTelemetry();
            if (telemetry.gpuTimingsValid)
            {
                terrainGpuMs += telemetry.gpuMilliseconds[static_cast<size_t>(
                    Engine::Graphics::GpuTimingStage::Terrain)];
                shadowGpuMs += telemetry.gpuMilliseconds[static_cast<size_t>(
                    Engine::Graphics::GpuTimingStage::Shadow)];
                ++gpuSamples;
            }
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
            << " afterJumpWorstMs=" << movingWorstMs
            << " renderCpuMs=" << renderCpuMs / 30.0
            << " ordinaryDraws=" << ordinaryDraws
            << " shadowDraws=" << shadowDraws
            << " shadowCandidates=" << shadowCandidates
            << " terrainGpuMs=" << (gpuSamples ? terrainGpuMs / gpuSamples : 0.0)
            << " shadowGpuMs=" << (gpuSamples ? shadowGpuMs / gpuSamples : 0.0)
            << " gpuSamples=" << gpuSamples << '\n';
        return 0;
    }
    for (int i = 0; i < 3; ++i) driver->Update();
    Require(manager->BakeProceduralCell(0, 0, true));
    Require(std::filesystem::file_size(manager->GetCacheFilePath(0, 0)) < 60000);
    manager->SyncLoadedCells(100, 100);
    Require(manager->GetLoadedCellCount() == 0);
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
    Require(manager->GetCellCount() == 16641);
    Require(manager->GetStoredTerrainCount() == 0);
    auto* center = manager->GetLoadedCell(0, 0);
    auto* neighbor = manager->GetLoadedCell(1, 0);
    Require(center && neighbor);
    Require(center->layers.size() == 6);
    Require(center->GetLayerMaterials().size() == 6);
    for (size_t layer = 0; layer < 6; ++layer)
    {
        const auto& material = center->GetLayerMaterials()[layer];
        Require(material && material->baseColorTexture &&
            material->baseColorTexture->GetGraphicsTexture());
    }
    bool foundBlendedHeight = false;
    for (size_t vertex = 0; vertex < center->heights.size(); ++vertex)
    {
        const float sampleHeight = center->heights[vertex];
        const size_t expected = TerrainHeightArchive::LayerForHeight(sampleHeight);
        float expectedWeights[4];
        TerrainHeightArchive::BlendBaseLayers(sampleHeight,
            static_cast<uint8_t>(expected), expectedWeights);
        const auto* mesh = center->Owner->GetComponent<Engine::Components::Mesh>();
        Require(mesh);
        constexpr size_t remappedLayers[4] = {4, 5, 2, 3};
        for (size_t layer = 0; layer < 4; ++layer)
        {
            const size_t destination = remappedLayers[layer];
            Require(std::abs(center->weights[vertex * 6 + destination] -
                expectedWeights[layer]) < .0001f);
            const float meshWeight = destination < 4
                ? mesh->GetVertices()[vertex].uvChannels[2][destination]
                : mesh->GetVertices()[vertex].uvChannels[3][destination - 4];
            Require(std::abs(meshWeight - expectedWeights[layer]) < .0001f);
        }
        Require(center->weights[vertex * 6] == 0.f &&
            center->weights[vertex * 6 + 1] == 0.f);
        foundBlendedHeight |= expectedWeights[expected] < .999f;
    }
    Require(foundBlendedHeight);
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
    manager->lod2Radius = 0;
    manager->SyncLoadedCells(0, 0);
    Require(manager->GetRenderBatchCount() > 0);
    Require(!neighbor->Owner->GetComponent<Engine::Components::Mesh>()->
        IsEditorVisible());
    renderer.BeginExportFrame(2);
    renderer.BeginFrame();
    renderer.Clear(.2f, .25f, .3f);
    auto batchContext = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(batchContext.get(), 1.f,
        camera->GetComponent<Engine::Components::Camera>(), false,
        width, height, true);
    const auto& shadowCulling = scene.GetShadowDebugSnapshot();
    Require(shadowCulling.submittedCasterDrawCount > 0);
    Require(shadowCulling.submittedCasterDrawCount <
        shadowCulling.casterCount * shadowCulling.cascadeCount);
    scene.Render(batchContext.get(), 1.f,
        camera->GetComponent<Engine::Components::Camera>(), false, width, height);
    Require(scene.GetLastOrdinaryDrawCount() < manager->GetLoadedCellCount());
    renderer.EndFrame();
    renderer.WaitIdle();
    std::vector<uint8_t> batchedPixels;
    Require(renderer.ReadExportFrameRGBA(batchedPixels, true));
    size_t visibleBatchPixels = 0;
    for (size_t i = 0; i + 3 < batchedPixels.size(); i += 4)
        visibleBatchPixels += batchedPixels[i] + batchedPixels[i + 1] +
            batchedPixels[i + 2] > 30;
    Require(visibleBatchPixels > 64);
    manager->lod2Radius = 1;
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
    manager->farRadius = 2;
    manager->cpuBudgetMiB = 1;
    manager->gpuBudgetMiB = 1;
    manager->SyncLoadedCells(0, 0);
    const auto bounded = manager->GetMemoryUsage();
    Require(bounded.cpuBytes <= 1024 * 1024);
    Require(bounded.gpuBytes <= 1024 * 1024);
    Require(manager->Serialize()["cpuBudgetMiB"].AsInt() == 1);
    Require(manager->Serialize()["gpuBudgetMiB"].AsInt() == 1);
    // Paint saved before the grass layers were appended still targets its
    // original dirt channel after the cell is loaded with six layers.
    {
        std::ofstream legacy(editPath / "0_0.terrainedit",
            std::ios::binary | std::ios::trunc);
        const uint64_t magic = 0x31544944454e5254ull;
        const uint32_t version = 1, side = 32, oldLayers = 4;
        const uint32_t noHeights = 0, oneWeight = 1, vertex = 0;
        const float oldPaint[4] = {1.f, 0.f, 0.f, 0.f};
        Require(TerrainHeightArchive::Write(legacy, magic) &&
            TerrainHeightArchive::Write(legacy, version) &&
            TerrainHeightArchive::Write(legacy, side) &&
            TerrainHeightArchive::Write(legacy, side) &&
            TerrainHeightArchive::Write(legacy, oldLayers) &&
            TerrainHeightArchive::Write(legacy, noHeights) &&
            TerrainHeightArchive::Write(legacy, oneWeight) &&
            TerrainHeightArchive::Write(legacy, vertex));
        for (float weight : oldPaint)
            Require(TerrainHeightArchive::Write(legacy, weight));
    }
    manager->farRadius = -1;
    manager->SyncLoadedCells(100, 100);
    manager->SyncLoadedCells(0, 0);
    auto* legacyCell = manager->GetLoadedCell(0, 0);
    Require(legacyCell);
    Require(legacyCell->weights[0] == 1.f &&
        legacyCell->weights[4] == 0.f && legacyCell->weights[5] == 0.f);
    std::filesystem::remove_all(editPath);
    // The legacy preview and far-grid checks use the archive source. Runtime
    // bake enforcement is covered by TerrainManagerTests.
    manager->requireRuntimeBake = false;
    manager->runtimeBakeDirectory.clear();
    scene.Start();
    driver->farGrid = patchedGrid.string();
    Require(driver->GetFarTileMeshes().empty());
    driver->Update();
    const auto farTiles = driver->GetFarTileMeshes();
    Require(farTiles.size() > 20 && farTiles.size() < 30);
    double farthestX = 0.0;
    float highestY = -1000.f;
    for (const auto* tile : farTiles)
    {
        Require(tile->GetIndexCount() > 0);
        farthestX = std::max(farthestX,
            tile->Owner->transform.GetWorldPositionD().x +
                tile->GetBoundsMax().x);
        highestY = std::max(highestY, tile->GetBoundsMax().y);
    }
    Require(farthestX > 8000.0 && highestY >= 100.f);
    auto* crossingCamera = scene.FindObjectByName(
        "Terrain Performance Camera");
    const int viewCellX = static_cast<int>(std::floor(
        crossingCamera->transform.GetWorldPositionD().x / 128.0));
    const int viewCellY = static_cast<int>(std::floor(
        crossingCamera->transform.GetWorldPositionD().z / 128.0));
    const auto checkFarSeam = [&](int nearX, int nearY, int edge)
    {
        std::vector<float> heights, weights;
        Require(TerrainHeightArchive::ReadCell(
            Engine::Components::Mesh::ResolveFilePath(driver->heightArchive),
            driver->SourceSignature(), driver->mapHalfExtentCells,
            manager->cellsX, manager->cellsY, nearX, nearY, heights,
            &weights));
        constexpr glm::vec3 baseColors[] = {
            {.04927f, .15253f, .03096f}, {.13832f, .22583f, .04636f},
            {.07028f, .17221f, .03527f}, {.21779f, .25200f, .07456f}};
        const bool vertical = edge == 0 || edge == 2;
        const int count = vertical ? manager->cellsY : manager->cellsX;
        for (int sample = 0; sample <= count; ++sample)
        {
            const int col = edge == 0 ? 0 : edge == 2 ? manager->cellsX : sample;
            const int row = edge == 3 ? 0 : edge == 1 ? manager->cellsY : sample;
            const double worldX = static_cast<double>(nearX) * 128.0 +
                col * manager->cellWidth;
            const double worldZ = static_cast<double>(nearY) * 128.0 +
                row * manager->cellDepth;
            const float expectedHeight = heights[static_cast<size_t>(
                row * (manager->cellsX + 1) + col)];
            const size_t weightIndex = static_cast<size_t>(
                row * (manager->cellsX + 1) + col) *
                TerrainHeightArchive::LayerCount;
            glm::vec3 expectedColor(0.f);
            for (int layer = 0; layer < TerrainHeightArchive::LayerCount;
                ++layer)
                expectedColor += baseColors[layer] *
                    weights[weightIndex + layer];
            const auto sampleHeight = [&](int x, int y) {
                return heights[static_cast<size_t>(
                    std::clamp(y, 0, manager->cellsY) *
                        (manager->cellsX + 1) +
                    std::clamp(x, 0, manager->cellsX))];
            };
            const glm::vec3 expectedNormal = glm::normalize(glm::vec3(
                (sampleHeight(col - 1, row) -
                    sampleHeight(col + 1, row)) /
                    (2.f * manager->cellWidth), 1.f,
                (sampleHeight(col, row - 1) -
                    sampleHeight(col, row + 1)) /
                    (2.f * manager->cellDepth)));
            bool matched = false;
            for (const auto* tile : farTiles)
            {
                const glm::dvec3 origin = tile->Owner->transform.GetWorldPositionD();
                const auto& vertices = tile->GetVertices();
                for (uint32_t index : tile->GetIndices())
                {
                    const auto& vertex = vertices[index];
                    if (std::abs(origin.x + vertex.pos[0] - worldX) < .001 &&
                        std::abs(origin.z + vertex.pos[2] - worldZ) < .001 &&
                        std::abs(vertex.pos[1] - expectedHeight) < .001f &&
                        glm::dot(glm::vec3(vertex.normal[0],
                            vertex.normal[1], vertex.normal[2]),
                            expectedNormal) > .9999f &&
                        glm::length(glm::vec3(vertex.color[0],
                            vertex.color[1], vertex.color[2]) -
                            expectedColor) < .0001f)
                    { matched = true; break; }
                }
                if (matched) break;
            }
            if (!matched)
                std::cerr << "Far seam mismatch cell=" << nearX << ',' << nearY
                    << " edge=" << edge << " sample=" << sample << '\n';
            Require(matched);
        }
    };
    const int detailedRadius = std::max(manager->loadedRadius,
        manager->farRadius);
    checkFarSeam(viewCellX + detailedRadius, viewCellY, 2);
    checkFarSeam(viewCellX - detailedRadius, viewCellY, 0);
    checkFarSeam(viewCellX, viewCellY + detailedRadius, 1);
    checkFarSeam(viewCellX, viewCellY - detailedRadius, 3);
    crossingCamera->transform.position = {-60.0 * 128.0 + 127.5,
        110.0, -60.0 * 128.0 + 64.0};
    driver->Update();
    const uint64_t uploadsBeforeCrossing = driver->GetFarTileUploadCount();
    crossingCamera->transform.position.x += 1.0;
    driver->Update();
    const uint64_t crossingUploads =
        driver->GetFarTileUploadCount() - uploadsBeforeCrossing;
    Require(crossingUploads > 0 && crossingUploads < 16);
    if (!benchmark)
    {
        auto* gameCameraObject = scene.FindObjectByName(
            "Terrain Performance Camera");
        auto* gameCamera = gameCameraObject->GetComponent<
            Engine::Components::Camera>();
        gameCameraObject->transform.position = {0.0, 110.0, -300.0};
        gameCameraObject->transform.rotation = {.38f, 0.f, 0.f};
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto gameContext = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(gameContext.get(), 1.f, gameCamera, false, width, height);
        renderer.EndFrame();
        renderer.WaitIdle();
        Require(scene.GetLastOrdinaryDrawCount() > 0);
        Require(scene.GetLastOrdinaryTriangleCount() >
            scene.GetLastOrdinaryDrawCount());
    }
    std::filesystem::remove_all(patchDirectory);
    // Play mode removes editor preview cells while the scene starts. Keep the
    // authored runtime-bake configuration here to exercise that transition.
    {
        Engine::Scene::Scene playScene;
        playScene.Init(renderer.GetGraphicsProvider());
        Require(playScene.Load(
            "Engine/Core/Assets/Scenes/terrain/terrain_performance.scene"));
        auto* playMap = playScene.FindObjectByName("Terrain Performance Map");
        Require(playMap != nullptr);
        auto* playManager = playMap->GetComponent<
            Engine::Components::TerrainManager>();
        Require(playManager && playManager->GetLoadedCellCount() > 0 &&
            playManager->requireRuntimeBake &&
            !playManager->runtimeBakeDirectory.empty());
        playScene.Start();
        Require(playScene.HasStarted() &&
            playManager->GetLoadedCellCount() == 0);
        auto* playDriver = playMap->GetComponent<TerrainPerformanceDriver>();
        Require(playDriver);
        playDriver->Update();
        const auto playFarTiles = playDriver->GetFarTileMeshes();
        Require(!playFarTiles.empty());
        auto* playCamera = playScene.FindObjectByName(
            "Terrain Performance Camera");
        Require(playCamera);
        const int cameraCellX = static_cast<int>(std::floor(
            playCamera->transform.GetWorldPositionD().x / 128.0));
        const int cameraCellY = static_cast<int>(std::floor(
            playCamera->transform.GetWorldPositionD().z / 128.0));
        const int nearCellX = cameraCellX +
            std::max(playManager->loadedRadius, playManager->farRadius);
        std::vector<float> bakedHeights, bakedWeights;
        uint32_t layerCount = 0;
        Require(playManager->ReadBakedCellHeights(
            {nearCellX, cameraCellY}, bakedHeights, &bakedWeights,
            &layerCount));
        Require(layerCount == 6 && bakedWeights.size() ==
            bakedHeights.size() * layerCount);
        constexpr glm::vec3 layerColors[] = {
            {.05856f, .04472f, .02751f}, {.19217f, .11348f, .05328f},
            {.07028f, .17221f, .03527f}, {.21779f, .25200f, .07456f},
            {.04927f, .15253f, .03096f}, {.13832f, .22583f, .04636f}};
        const int seamRow = playManager->cellsY / 2;
        const size_t sample = static_cast<size_t>(seamRow) *
            (playManager->cellsX + 1) + playManager->cellsX;
        glm::vec3 expectedColor(0.f);
        for (uint32_t layer = 0; layer < layerCount; ++layer)
            expectedColor += layerColors[layer] *
                bakedWeights[sample * layerCount + layer];
        const double worldX = static_cast<double>(nearCellX + 1) * 128.0;
        const double worldZ = static_cast<double>(cameraCellY) * 128.0 +
            seamRow * playManager->cellDepth;
        bool matchedColor = false;
        for (const auto* tile : playFarTiles)
        {
            const glm::dvec3 origin =
                tile->Owner->transform.GetWorldPositionD();
            const auto& vertices = tile->GetVertices();
            for (uint32_t index : tile->GetIndices())
            {
                const auto& vertex = vertices[index];
                if (std::abs(origin.x + vertex.pos[0] - worldX) < .001 &&
                    std::abs(origin.z + vertex.pos[2] - worldZ) < .001 &&
                    std::abs(vertex.pos[1] - bakedHeights[sample]) < .001f &&
                    glm::length(glm::vec3(vertex.color[0],
                        vertex.color[1], vertex.color[2]) - expectedColor) <
                        .0001f)
                { matchedColor = true; break; }
            }
            if (matchedColor) break;
        }
        Require(matchedColor);
    }
    // Exercise the optional terrain material slots with real GPU textures.
    // The bundled landscape uses albedo maps only, so borrow an existing map
    // here to verify normal, roughness, and height binding through a draw.
    manager->SyncLoadedCells(0, 0);
    auto* materialCell = manager->GetLoadedCell(0, 0);
    Require(materialCell && !materialCell->GetLayerMaterials().empty());
    auto layerMaterial = materialCell->GetLayerMaterials().front();
    Require(layerMaterial && layerMaterial->baseColorTexture);
    const std::string testMap = layerMaterial->baseColorTexture->GetFilePath();
    layerMaterial->SetNormalTexture(testMap);
    layerMaterial->SetMetallicRoughnessTexture(testMap);
    layerMaterial->SetHeightTexture(testMap);
    layerMaterial->terrainHeightBlendStrength = .4f;
    layerMaterial->PrepareTextures(renderer.GetGraphicsProvider());
    Require(layerMaterial->normalTexture &&
        layerMaterial->normalTexture->GetGraphicsTexture() &&
        layerMaterial->metallicRoughnessTexture &&
        layerMaterial->metallicRoughnessTexture->GetGraphicsTexture() &&
        layerMaterial->heightTexture &&
        layerMaterial->heightTexture->GetGraphicsTexture());
    crossingCamera->transform.position = {64.0, 100.0, -80.0};
    crossingCamera->transform.rotation = {.7f, 0.f, 0.f};
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    auto materialContext = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(materialContext.get(), 1.f,
        crossingCamera->GetComponent<Engine::Components::Camera>(),
        false, width, height);
    renderer.EndFrame();
    renderer.WaitIdle();
    Require(scene.GetLastOrdinaryDrawCount() > 0);
}
