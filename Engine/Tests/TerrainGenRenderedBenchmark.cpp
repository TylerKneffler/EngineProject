#include "Core/Assets/Scripts/TerrainGen/TerrainChunk.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainGen.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Object.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

void PumpWindowMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

double Percentile(std::vector<double> values, double percentile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::round(
        percentile * static_cast<double>(values.size() - 1u)));
    return values[std::min(index, values.size() - 1u)];
}
}

int main(int argumentCount, char** arguments)
{
    try
    {
        const int radius = argumentCount > 1
            ? std::clamp(std::atoi(arguments[1]), 0, 8) : 8;
        constexpr uint32_t width = 1280;
        constexpr uint32_t height = 720;
        constexpr int measuredFrames = 600;

        Engine::Model::ProjectSettings settings{};
        settings.gameRenderingAPI = "DirectX11";
        Engine::Core::Window window(GetModuleHandleW(nullptr),
            L"TerrainGen Rendered Benchmark", width, height);
        auto renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
        if (!renderer || !renderer->Init(window.GetHWND(), width, height))
            throw std::runtime_error("DirectX 11 renderer initialization failed");

        Engine::Scene::Scene scene;
        scene.Init(renderer->GetGraphicsProvider());
        if (!scene.Load(
            "Engine/Core/Assets/Scenes/Procedural/terrain_gen_streaming.scene"))
            throw std::runtime_error("Terrain streaming scene failed to load");

        auto* terrainObject = scene.FindObjectByName("Long Range Terrain");
        auto* cameraObject = scene.FindObjectByName("Long Range Terrain Camera");
        auto* terrain = terrainObject
            ? terrainObject->GetComponent<TerrainGen>() : nullptr;
        auto* driver = cameraObject
            ? cameraObject->GetComponent<TerrainStreamingCameraDriver>() : nullptr;
        if (!terrain || !driver)
            throw std::runtime_error("Terrain benchmark components are missing");
        terrain->viewRadiusInChunks = radius;
        scene.Start();
        const float traversalSpeed = driver->movementSpeed;
        driver->movementSpeed = 0.01f;

        ShowWindow(window.GetHWND(), SW_SHOWNOACTIVATE);
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const auto renderFrame = [&]()
        {
            const auto start = Clock::now();
            PumpWindowMessages();
            scene.Update(1.f / 60.f);
            scene.PrepareRenderFrame();
            renderer->BeginFrame();
            renderer->Clear(0.1f, 0.1f, 0.1f);
            auto context = renderer->CreateFrameGraphicsContext();
            if (context)
            {
                if (auto* camera = scene.FindGameCamera())
                    scene.Render(context.get(), aspect, camera, false);
            }
            renderer->EndFrame();
            return std::chrono::duration<double, std::milli>(
                Clock::now() - start).count();
        };

        const size_t diameter = static_cast<size_t>(radius * 2 + 1);
        const size_t desiredChunks = diameter * diameter;
        const auto fillStart = Clock::now();
        double fillWorstMs = 0.0;
        while (terrain->GetLoadedChunkCount() != desiredChunks ||
            terrain->GetQueuedChunkCount() != 0u ||
            terrain->GetInFlightChunkCount() != 0u)
        {
            fillWorstMs = std::max(fillWorstMs, renderFrame());
            if (Clock::now() - fillStart > std::chrono::seconds(90))
                throw std::runtime_error("Initial terrain fill timed out");
        }
        const double fillSeconds = std::chrono::duration<double>(
            Clock::now() - fillStart).count();

        driver->movementSpeed = traversalSpeed;
        std::vector<double> frameTimes;
        frameTimes.reserve(measuredFrames);
        size_t maximumMissingChunks = 0u;
        int viewerChunkMissingFrames = 0;
        int forwardChunkMissingFrames = 0;
        glm::vec3 travelDirection = driver->endPosition - driver->startPosition;
        travelDirection.y = 0.f;
        travelDirection = glm::normalize(travelDirection);
        for (int frame = 0; frame < measuredFrames; ++frame)
        {
            frameTimes.push_back(renderFrame());
            const size_t loaded = terrain->GetLoadedChunkCount();
            maximumMissingChunks = std::max(maximumMissingChunks,
                desiredChunks > loaded ? desiredChunks - loaded : 0u);
            const glm::vec3 viewerPosition = cameraObject->transform.GetWorldPosition();
            const int viewerX = static_cast<int>(std::floor(
                viewerPosition.x / terrain->chunkSize));
            const int viewerZ = static_cast<int>(std::floor(
                viewerPosition.z / terrain->chunkSize));
            if (!terrain->IsChunkLoaded(viewerX, viewerZ))
                ++viewerChunkMissingFrames;
            const glm::vec3 forwardPosition = viewerPosition +
                travelDirection * terrain->chunkSize;
            const int forwardX = static_cast<int>(std::floor(
                forwardPosition.x / terrain->chunkSize));
            const int forwardZ = static_cast<int>(std::floor(
                forwardPosition.z / terrain->chunkSize));
            if (!terrain->IsChunkLoaded(forwardX, forwardZ))
                ++forwardChunkMissingFrames;
        }

        size_t residentVertices = 0u;
        for (Engine::Core::Object* chunkObject : terrainObject->Children)
        {
            if (!chunkObject || !chunkObject->GetComponent<TerrainChunk>())
                continue;
            for (Engine::Core::Object* patchObject : chunkObject->Children)
            {
                const auto* mesh = patchObject
                    ? patchObject->GetComponent<Engine::Components::Mesh>() : nullptr;
                if (mesh)
                    residentVertices += mesh->GetVertexCount();
            }
        }

        const double averageMs = std::accumulate(frameTimes.begin(),
            frameTimes.end(), 0.0) / static_cast<double>(frameTimes.size());
        const int overBudget = static_cast<int>(std::count_if(frameTimes.begin(),
            frameTimes.end(), [](double milliseconds)
            {
                return milliseconds > 16.667;
            }));
        std::printf("rendered api=DirectX11 resolution=%ux%u radius=%d chunks=%zu "
            "view_distance=%.0f fill_s=%.3f fill_worst_ms=%.3f avg_ms=%.3f "
            "p95_ms=%.3f p99_ms=%.3f worst_ms=%.3f over_budget=%d/%d "
            "max_missing=%zu viewer_missing=%d forward_missing=%d vertices=%zu "
            "built=%llu unloaded=%llu\n",
            width, height, radius, desiredChunks,
            terrain->chunkSize * static_cast<float>(radius), fillSeconds,
            fillWorstMs, averageMs, Percentile(frameTimes, 0.95),
            Percentile(frameTimes, 0.99),
            *std::max_element(frameTimes.begin(), frameTimes.end()), overBudget,
            measuredFrames, maximumMissingChunks, viewerChunkMissingFrames,
            forwardChunkMissingFrames, residentVertices,
            static_cast<unsigned long long>(terrain->GetTotalChunksBuilt()),
            static_cast<unsigned long long>(terrain->GetTotalChunksUnloaded()));
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "rendered terrain benchmark failed: %s\n", error.what());
        return 1;
    }
}
