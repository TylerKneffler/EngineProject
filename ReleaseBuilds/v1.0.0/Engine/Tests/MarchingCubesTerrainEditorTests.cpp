#include "Core/Assets/Scripts/MarchingCubes/MarchingCubesChunk.h"
#include "Core/Assets/Scripts/MarchingCubes/MarchingCubesQuad.h"
#include "Core/Assets/Scripts/MarchingCubes/MarchingCubesTerrain.h"
#include "Core/Assets/Scripts/MarchingCubes/PerlinNoiseField.h"
#include "Core/Assets/Scripts/MarchingCubes/TerrainStreamingCameraDriver.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>
#include <thread>

int main()
{
    Engine::Scene::Scene scene;
    Engine::Core::Object* terrainObject = scene.AddObject("Editor Terrain");
    terrainObject->AddComponent<PerlinNoiseField>();
    MarchingCubesTerrain* terrain =
        terrainObject->AddComponent<MarchingCubesTerrain>();
    terrain->viewerObjectName.clear();
    terrain->viewRadiusInChunks = 1;
    terrain->horizontalCellsPerChunk = 4;
    terrain->verticalCells = 4;
    terrain->quadsPerAxis = 2;
    terrain->generateColliders = false;

    // This deliberately does not call Scene::Start(). It exercises the same
    // immediate path used by the component's editor button.
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 9u ||
        terrainObject->Children.size() != 9u)
    {
        std::fprintf(stderr, "Editor generation did not create the 3x3 chunk ring\n");
        return 1;
    }

    std::size_t quadCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        MarchingCubesChunk* chunk = chunkObject
            ? chunkObject->GetComponent<MarchingCubesChunk>() : nullptr;
        if (!chunk || chunk->quadCount != 4 || chunkObject->Children.size() != 4u)
            return 2;
        for (Engine::Core::Object* quadObject : chunkObject->Children)
        {
            MarchingCubesQuad* quad = quadObject
                ? quadObject->GetComponent<MarchingCubesQuad>() : nullptr;
            Engine::Components::Mesh* mesh = quadObject
                ? quadObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!quad || !mesh ||
                !quadObject->GetComponent<Engine::Components::Material>() ||
                static_cast<std::size_t>(quad->triangleCount) * 3u !=
                    mesh->GetVertexCount())
            {
                return 3;
            }
            ++quadCount;
        }
    }
    if (quadCount != 36u)
        return 4;

    // Orthogonal mode may triangulate quads for the GPU, but every triangle
    // must lie on an axis-aligned plane and use one of the selected height
    // range colors. No diagonal terrain connection is permitted.
    terrain->viewRadiusInChunks = 0;
    terrain->horizontalCellsPerChunk = 6;
    terrain->quadsPerAxis = 2;
    terrain->geometryMode = static_cast<int>(
        MarchingCubesTerrain::GeometryMode::OrthogonalQuads);
    terrain->orthogonalHeightStep = 1.f;
    terrain->lowHeightColor = { 1.f, 0.f, 0.f };
    terrain->middleHeightColor = { 0.f, 1.f, 0.f };
    terrain->highHeightColor = { 0.f, 0.f, 1.f };
    if (!terrain->GenerateTerrain())
        return 5;
    std::size_t orthogonalVertexCount = 0;
    for (Engine::Core::Object* chunkObject : terrainObject->Children)
    {
        for (Engine::Core::Object* quadObject : chunkObject->Children)
        {
            Engine::Components::Mesh* mesh = quadObject
                ? quadObject->GetComponent<Engine::Components::Mesh>() : nullptr;
            if (!mesh)
                return 6;
            const auto& vertices = mesh->GetVertices();
            orthogonalVertexCount += vertices.size();
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
    if (orthogonalVertexCount == 0u)
        return 10;

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

    Engine::Scene::Scene performanceScene;
    if (!Engine::Serialization::SceneSerializer::Load(performanceScene,
        "Engine/Core/Assets/Scenes/Procedural/orthogonal_quad_terrain_streaming.scene",
        nullptr))
        return 14;
    auto* loadedTerrainObject = performanceScene.FindObjectByName(
        "Orthogonal Quad Terrain");
    auto* loadedCameraObject = performanceScene.FindObjectByName(
        "Orthogonal Terrain Streaming Camera");
    auto* loadedTerrain = loadedTerrainObject
        ? loadedTerrainObject->GetComponent<MarchingCubesTerrain>() : nullptr;
    auto* loadedDriver = loadedCameraObject
        ? loadedCameraObject->GetComponent<TerrainStreamingCameraDriver>() : nullptr;
    if (!loadedTerrain || !loadedCameraObject || !loadedDriver ||
        static_cast<MarchingCubesTerrain::GeometryMode>(loadedTerrain->geometryMode) !=
            MarchingCubesTerrain::GeometryMode::OrthogonalQuads)
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
    terrain->quadsPerAxis = 1;
    if (!terrain->GenerateTerrain() || terrain->GetLoadedChunkCount() != 1u ||
        terrainObject->Children.size() != 1u ||
        terrainObject->Children.front()->Children.size() != 1u)
    {
        std::fprintf(stderr, "Editor regeneration left stale generated objects\n");
        return 17;
    }

    std::printf("marching_cubes_editor_generation chunks=1 quads=1 "
        "initial_chunks=9 initial_quads=%zu orthogonal_vertices=%zu "
        "streamed_builds=%llu streamed_unloads=%llu max_stream_ms=%.3f "
        "runtime_streaming_tested=true\n",
        quadCount, orthogonalVertexCount,
        static_cast<unsigned long long>(streamedBuilds),
        static_cast<unsigned long long>(streamedUnloads),
        maximumStreamingMilliseconds);
    return 0;
}
