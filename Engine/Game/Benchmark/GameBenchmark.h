#pragma once

#include "Game/Application/GameApplication.h"
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Scene/Scene.h"
#include <fstream>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Renderers { class IGameRenderer; }

namespace Engine::Game
{
struct GameBenchmarkSceneSample
{
    Engine::Rendering::RenderCpuTimings render;
    Engine::Scene::Scene::ShadowCpuTiming shadow;
    Engine::Scene::Scene::PrepareTimings preparation;
    uint64_t pendingPrefabLayers = 0;
    uint64_t sharedPrefabVertices = 0, sharedPrefabIndices = 0, sharedPrefabGeometryBytes = 0;
    uint64_t objects = 0, meshes = 0, terrains = 0, placements = 0;
    uint64_t storedCells = 0, loadedCells = 0, preparingCells = 0;
    uint64_t loadedPrefabCells = 0, pooledPrefabCells = 0;
    uint64_t sceneGeneration = 0;
    bool camera = false;
    double cameraPosition[3]{};
};

struct GameBenchmarkFrameSample
{
    double intervalMs = 0, updateMs = 0, prepareMs = 0, recordMs = 0;
    double packetPrepareMs = 0, shadowRecordMs = 0, cameraRecordMs = 0, resourceRetainMs = 0;
    double updateWaitMs = 0, prepareWaitMs = 0, replayMs = 0, endFrameMs = 0;
    double renderThreadMs = 0, latencyMs = 0, metricsCollectionMs = 0, residencyWaitMs = 0;
    uint32_t width = 0, height = 0;
    float renderScale = 1.f;
    Engine::Graphics::FrameTimingTelemetry gpu;
    GameBenchmarkSceneSample scene;
};

class GameBenchmark
{
public:
    GameBenchmark(const GameBenchmarkOptions& options,
        Engine::Renderers::IGameRenderer& renderer, const std::string& scene,
        uint32_t maxFps, bool clusteredLighting, double startupMs);
    static GameBenchmarkSceneSample CollectScene(Engine::Scene::Scene& scene);
    void Record(uint64_t frame, const GameBenchmarkFrameSample& sample);
    void Capture(Engine::Renderers::IGameRenderer& renderer);
    bool Finish();
    const std::filesystem::path& Directory() const { return m_directory; }

private:
    GameBenchmarkOptions m_options;
    std::filesystem::path m_directory;
    std::ofstream m_log;
    std::vector<double> m_intervals;
    uint64_t m_peakDraws = 0, m_peakTriangles = 0;
    uint64_t m_peakTerrains = 0, m_peakLoadedCells = 0;
    uint64_t m_validFrameCount = 0;
    double m_cameraOrigin[3]{}, m_maxCameraDisplacement = 0.0;
    bool m_cameraOriginSet = false;
    bool m_cameraSeen = false, m_validFrameSeen = false, m_captureWritten = false;
};
}
