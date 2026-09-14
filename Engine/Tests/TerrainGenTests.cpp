#include "Core/Assets/Scripts/TerrainGen/TerrainChunk.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainPatch.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainGen.h"
#include "Core/Assets/Scripts/TerrainGen/PerlinNoiseField.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argumentCount, char** arguments)
{
    Engine::Scene::Scene scene;
    Engine::Core::Object* terrainObject = scene.AddObject("Editor Terrain");
    terrainObject->AddComponent<PerlinNoiseField>();
    TerrainGen* terrain =
        terrainObject->AddComponent<TerrainGen>();
    terrain->viewerObjectName.clear();
    terrain->viewRadiusInChunks = 1;
    terrain->horizontalCellsPerChunk = 4;
    terrain->verticalCells = 4;
    terrain->patchesPerAxis = 2;
    terrain->generateColliders = false;

    // This deliberately does not call Scene::Start(). It exercises the same
    // immediate path used by the component's editor button.
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u ||
        terrainObject->Children.size() != 9u)
    {
        std::fprintf(stderr, "Editor generation did not create the 3x3 chunk ring\n");
        return 1;
    }

    std::size_t patchCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        TerrainChunk* chunk = chunkObject
            ? chunkObject->GetComponent<TerrainChunk>() : nullptr;
        if (!chunk || chunk->patchCount != 4 || chunkObject->Children.size() != 4u)
            return 2;
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            TerrainPatch* patch = patchObject
                ? patchObject->GetComponent<TerrainPatch>() : nullptr;
            Engine::Components::Mesh* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!patch || !mesh ||
                !patchObject->GetComponent<Engine::Components::Material>() ||
                static_cast<std::size_t>(patch->triangleCount) * 3u !=
                    mesh->GetVertexCount())
            {
                return 3;
            }
            ++patchCount;
        }
    }
    if (patchCount != 36u)
        return 4;

    // Cube mode may triangulate square faces for the GPU, but every triangle
    // must lie on an axis-aligned plane and use one of the selected height
    // range colors. No diagonal terrain connection is permitted.
    terrain->viewRadiusInChunks = 0;
    terrain->horizontalCellsPerChunk = 6;
    terrain->patchesPerAxis = 2;
    terrain->terrainShape = static_cast<int>(
        TerrainGen::TerrainShape::Cubes);
    terrain->heightStep = 1.f;
    terrain->lowHeightColor = { 1.f, 0.f, 0.f };
    terrain->middleHeightColor = { 0.f, 1.f, 0.f };
    terrain->highHeightColor = { 0.f, 0.f, 1.f };
    if (!terrain->GenerateTerrain())
        return 5;
    std::size_t cubeVertexCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        for (Engine::Core::Object* patchObject : chunkObject->Children)
        {
            Engine::Components::Mesh* mesh = patchObject
                ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                return 6;
            const auto& vertices = mesh->GetVertices();
            cubeVertexCount += vertices.size();
            for (std::size_t index = 0; index + 2 < vertices.size(); index += 3)
            {
                const auto& vertex = vertices[index];
                const glm::vec3 normal(vertex.normal[0], vertex.normal[1],
                    vertex.normal[2]);
                const glm::vec3 absoluteNormal = glm::abs(normal);
                const int axisCount = static_cast<int>(absoluteNormal.x > 0.999f) +
                    static_cast<int>(absoluteNormal.y > 0.999f) +
                    static_cast<int>(absoluteNormal.z > 0.999f);
                if (axisCount != 1 || std::abs(glm::length(normal) - 1.f) > 0.001f)
                    return 7;
                const glm::vec3 first(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
                const auto& secondVertex = vertices[index + 1];
                const auto& thirdVertex = vertices[index + 2];
                const glm::vec3 second(secondVertex.pos[0], secondVertex.pos[1],
                    secondVertex.pos[2]);
                const glm::vec3 third(thirdVertex.pos[0], thirdVertex.pos[1],
                    thirdVertex.pos[2]);
                if (std::abs(glm::dot(second - first, normal)) > 0.001f ||
                    std::abs(glm::dot(third - first, normal)) > 0.001f)
                    return 8;
                const glm::vec3 color(vertex.color[0], vertex.color[1],
                    vertex.color[2]);
                const bool selectedBand = glm::length(color - terrain->lowHeightColor) < 0.001f ||
                    glm::length(color - terrain->middleHeightColor) < 0.001f ||
                    glm::length(color - terrain->highHeightColor) < 0.001f;
                if (!selectedBand)
                    return 9;
            }
        }
    }
    if (cubeVertexCount == 0u)
        return 10;

    // The generalized generator must produce distinct, valid mesh topology
    // for both a triangulated height field and hexagonal columns.
    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Triangles);
    terrain->patchesPerAxis = 1;
    if (!terrain->GenerateTerrain())
        return 18;
    const auto* triangleMesh = terrainObject->Children.front()->Children.front()
        ->GetComponent<Engine::Components::Mesh>();
    if (!triangleMesh || triangleMesh->GetVertexCount() !=
        static_cast<std::size_t>(terrain->horizontalCellsPerChunk *
            terrain->horizontalCellsPerChunk * 6))
        return 19;

    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Hexagons);
    if (!terrain->GenerateTerrain())
        return 20;
    const auto* hexMesh = terrainObject->Children.front()->Children.front()
        ->GetComponent<Engine::Components::Mesh>();
    bool foundHexSide = false;
    if (!hexMesh || hexMesh->GetVertexCount() == 0u ||
        hexMesh->GetVertexCount() % 3u != 0u)
        return 21;
    for (const auto& vertex : hexMesh->GetVertices())
    {
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (std::abs(normal.y) < 0.001f && std::abs(normal.x) > 0.1f &&
            std::abs(normal.z) > 0.1f)
            foundHexSide = true;
    }
    if (!foundHexSide)
        return 22;

    terrain->terrainShape = static_cast<int>(TerrainGen::TerrainShape::Cubes);
    terrain->patchesPerAxis = 2;

    Engine::Core::Object* viewer = scene.AddObject("Streaming Test Viewer");
    terrain->viewerObjectName = viewer->name;
    terrain->viewRadiusInChunks = 1;
    terrain->maxChunkBuildsPerUpdate = 16;
    viewer->transform.position = { 0.f, 10.f, 0.f };
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u)
        return 11;
    scene.Start();
    const auto waitForStreaming = [&scene, terrain](std::size_t expectedChunks)
    {
        for (int update = 0; update < 500; ++update)
        {
            scene.Update(1.f / 60.f);
            if (terrain->GetLoadedChunkCount() == expectedChunks &&
                terrain->GetQueuedChunkCount() == 0u &&
                terrain->GetInFlightChunkCount() == 0u)
                return true;
            std::this_thread::yield();
        }
        return false;
    };
    viewer->transform.position.x = terrain->chunkSize * 3.f;
    if (!waitForStreaming(9u) ||
        terrain->GetTotalChunksBuilt() < 18u ||
        terrain->GetTotalChunksUnloaded() < 9u ||
        !std::isfinite(terrain->GetLastStreamingMilliseconds()) ||
        terrain->GetMaximumStreamingMilliseconds() <
            terrain->GetLastStreamingMilliseconds())
        return 12;
    viewer->transform.position.x = 0.f;
    if (!waitForStreaming(9u) ||
        terrain->GetMeshCacheHits() < 9u ||
        terrain->GetCachedChunkCount() == 0u)
    {
        std::fprintf(stderr, "Cache return failed: loaded=%zu queued=%zu in_flight=%zu hits=%llu cached=%zu\n",
            terrain->GetLoadedChunkCount(), terrain->GetQueuedChunkCount(),
            terrain->GetInFlightChunkCount(),
            static_cast<unsigned long long>(terrain->GetMeshCacheHits()),
            terrain->GetCachedChunkCount());
        return 13;
    }

    terrain->ClearTerrain();
    if (terrain->GetLoadedChunkCount() != 0u ||
        terrain->GetCachedChunkCount() != 0u ||
        terrain->GetQueuedChunkCount() != 0u ||
        terrain->GetInFlightChunkCount() != 0u ||
        !terrainObject->Children.empty())
        return 23;
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u)
        return 24;

    Engine::Scene::Scene performanceScene;
    if (!Engine::Serialization::SceneSerializer::Load(performanceScene,
        "Engine/Core/Assets/Scenes/Procedural/terrain_gen_streaming.scene",
        nullptr))
        return 14;
    auto* loadedTerrainObject = performanceScene.FindObjectByName(
        "Cube Terrain");
    auto* loadedCameraObject = performanceScene.FindObjectByName(
        "Cube Terrain Streaming Camera");
    auto* loadedTerrain = loadedTerrainObject
        ? loadedTerrainObject->GetComponent<TerrainGen>() : nullptr;
    auto* loadedDriver = loadedCameraObject
        ? loadedCameraObject->GetComponent<TerrainStreamingCameraDriver>() : nullptr;
    if (!loadedTerrain || !loadedCameraObject || !loadedDriver ||
        static_cast<TerrainGen::TerrainShape>(loadedTerrain->terrainShape) !=
            TerrainGen::TerrainShape::Cubes)
        return 15;
    loadedDriver->Start();
    const glm::vec3 cameraStart = loadedCameraObject->transform.position;
    loadedDriver->Update();
    const glm::vec3 cameraDelta = loadedCameraObject->transform.position - cameraStart;
    glm::vec3 expectedDirection = loadedDriver->endPosition -
        loadedDriver->startPosition;
    expectedDirection.y = 0.f;
    expectedDirection = glm::normalize(expectedDirection);
    const glm::vec3 cameraForward = glm::normalize(glm::vec3(
        loadedCameraObject->transform.GetWorldMatrix()[2]));
    if (!loadedDriver->moveInfinitely ||
        glm::dot(cameraDelta, expectedDirection) <= 0.f ||
        glm::dot(glm::normalize(glm::vec3(expectedDirection.x,
            -loadedDriver->downwardLook, expectedDirection.z)),
            cameraForward) < 0.999f)
        return 16;

    const uint64_t streamedBuilds = terrain->GetTotalChunksBuilt();
    const uint64_t streamedUnloads = terrain->GetTotalChunksUnloaded();
    const double maximumStreamingMilliseconds =
        terrain->GetMaximumStreamingMilliseconds();

    terrain->viewRadiusInChunks = 0;
    terrain->patchesPerAxis = 1;
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 1u ||
        terrainObject->Children.size() != 1u ||
        terrainObject->Children.front()->Children.size() != 1u)
    {
        std::fprintf(stderr, "Editor regeneration left stale generated objects\n");
        return 17;
    }

    if (argumentCount > 1 && std::strcmp(arguments[1], "--stress") == 0)
    {
        Engine::Scene::Scene stressScene;
        auto* stressViewer = stressScene.AddObject("Stress Viewer");
        auto* stressOwner = stressScene.AddObject("Stress Terrain");
        stressOwner->AddComponent<PerlinNoiseField>();
        auto* stressTerrain = stressOwner->AddComponent<TerrainGen>();
        stressTerrain->viewerObjectName = stressViewer->name;
        stressTerrain->chunkSize = 32.f;
        stressTerrain->viewRadiusInChunks = 4;
        stressTerrain->maxChunkBuildsPerUpdate = 16;
        stressTerrain->parallelChunkBuilds = 8;
        stressTerrain->maxChunkCommitsPerUpdate = 4;
        stressTerrain->horizontalCellsPerChunk = 32;
        stressTerrain->verticalCells = 32;
        stressTerrain->patchesPerAxis = 2;
        TerrainGen::TerrainShape stressShape = TerrainGen::TerrainShape::SmoothSurface;
        const char* stressShapeName = "smooth";
        if (argumentCount > 2 && std::strcmp(arguments[2], "cubes") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Cubes;
            stressShapeName = "cubes";
        }
        else if (argumentCount > 2 && std::strcmp(arguments[2], "triangles") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Triangles;
            stressShapeName = "triangles";
        }
        else if (argumentCount > 2 && std::strcmp(arguments[2], "hexagons") == 0)
        {
            stressShape = TerrainGen::TerrainShape::Hexagons;
            stressShapeName = "hexagons";
        }
        stressTerrain->terrainShape = static_cast<int>(stressShape);
        stressTerrain->caveStrength = 2.1f;
        stressTerrain->unloadedMeshCacheCapacity = 256;
        stressViewer->transform.position = { 0.f, 12.f, 0.f };
        stressScene.Start();

        constexpr std::size_t desiredChunks = 81u;
        const auto fillStart = std::chrono::steady_clock::now();
        while ((stressTerrain->GetLoadedChunkCount() != desiredChunks ||
            stressTerrain->GetQueuedChunkCount() != 0u ||
            stressTerrain->GetInFlightChunkCount() != 0u) &&
            std::chrono::steady_clock::now() - fillStart < std::chrono::seconds(60))
        {
            stressScene.Update(1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double initialFillSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - fillStart).count();

        double worstUpdateMs = 0.0;
        int updatesOverBudget = 0;
        std::size_t maximumMissingChunks = 0u;
        constexpr int movementFrames = 600;
        constexpr float cameraSpeed = 96.f;
        auto nextFrame = std::chrono::steady_clock::now();
        for (int frame = 0; frame < movementFrames; ++frame)
        {
            nextFrame += std::chrono::milliseconds(16);
            stressViewer->transform.position.x += cameraSpeed / 60.f;
            const auto updateStart = std::chrono::steady_clock::now();
            stressScene.Update(1.f / 60.f);
            const double updateMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - updateStart).count();
            worstUpdateMs = std::max(worstUpdateMs, updateMs);
            if (updateMs > 16.667)
                ++updatesOverBudget;
            maximumMissingChunks = std::max(maximumMissingChunks,
                desiredChunks > stressTerrain->GetLoadedChunkCount()
                    ? desiredChunks - stressTerrain->GetLoadedChunkCount() : 0u);
            std::this_thread::sleep_until(nextFrame);
        }
        const auto drainStart = std::chrono::steady_clock::now();
        while ((stressTerrain->GetLoadedChunkCount() != desiredChunks ||
            stressTerrain->GetQueuedChunkCount() != 0u ||
            stressTerrain->GetInFlightChunkCount() != 0u) &&
            std::chrono::steady_clock::now() - drainStart < std::chrono::seconds(60))
        {
            stressScene.Update(1.f / 60.f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double drainSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - drainStart).count();
        std::printf("stress shape=%s initial_fill_s=%.3f camera_speed=%.1f chunk_size=%.1f "
            "radius=%d cells=32x32 worst_update_ms=%.3f over_budget=%d/%d "
            "max_missing=%zu drain_s=%.3f built=%llu unloaded=%llu\n",
            stressShapeName, initialFillSeconds, cameraSpeed, stressTerrain->chunkSize,
            stressTerrain->viewRadiusInChunks, worstUpdateMs, updatesOverBudget,
            movementFrames, maximumMissingChunks, drainSeconds,
            static_cast<unsigned long long>(stressTerrain->GetTotalChunksBuilt()),
            static_cast<unsigned long long>(stressTerrain->GetTotalChunksUnloaded()));
    }

    std::printf("terrain_gen_generation chunks=1 patches=1 "
        "initial_chunks=9 initial_patches=%zu cube_vertices=%zu "
        "streamed_builds=%llu streamed_unloads=%llu max_stream_ms=%.3f "
        "runtime_streaming_tested=true\n",
        patchCount, cubeVertexCount,
        static_cast<unsigned long long>(streamedBuilds),
        static_cast<unsigned long long>(streamedUnloads),
        maximumStreamingMilliseconds);
    return 0;
}
