#pragma once

#include "Game/Application/GameApplication.h"
#include "Core/Rendering/RenderPerformance.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Scene/Scene.h"
#include "Core/Rendering/SceneRenderer.h"
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Renderers { class IGameRenderer; }

namespace Engine::Game
{
struct GameBenchmarkSceneSample
{
    Engine::Rendering::RenderCpuTimings render;
    Engine::Rendering::SceneRenderer::ShadowCpuTiming shadow;
    Engine::Rendering::SceneRenderer::PrepareTimings preparation;
    uint64_t pendingPrefabLayers = 0;
    uint64_t sharedPrefabVertices = 0, sharedPrefabIndices = 0, sharedPrefabGeometryBytes = 0;
    uint64_t objects = 0, meshes = 0, terrains = 0, placements = 0;
    uint64_t storedCells = 0, loadedCells = 0, preparingCells = 0;
    // Terrain streaming phases of the first manager (milliseconds).
    double streamTotalMs = 0, streamUnloadMs = 0, streamScheduleMs = 0, streamLodMs = 0,
        streamCommitMs = 0, streamCommitSceneMs = 0, streamCommitGraphicsMs = 0,
        streamCommitPhysicsMs = 0, streamColliderMs = 0, streamBatchMs = 0,
        streamTrimMs = 0, streamFarObjectsMs = 0;
    double streamCommitTerrainMs = 0, streamCommitLayersMs = 0, streamCommitRestoreMs = 0;
    double updateObjectsMs = 0, updatePhysicsMs = 0, updatePostPhysicsMs = 0;
    // Script and plugin counters (Scene::GetCounters).
    std::vector<Engine::Scene::SceneCounter> counters;
    uint64_t loadedPrefabCells = 0, pooledPrefabCells = 0;
    // Components/Terrain/TerrainEvents.h, taken this frame.
    uint32_t terrainChunkLevelChanges = 0, terrainChunkSeamChanges = 0, terrainStepSwaps = 0;
    uint32_t terrainFarSwaps = 0, terrainFarNodes = 0, terrainCellHandovers = 0;
    // TerrainEvents::TakeTransitions: object representation changes (transition_*).
    std::vector<std::pair<const char*, double>> transitions;
    // Far objects of all managers; settled when every one reports settled.
    uint64_t farObjectCells = 0, farObjectQueuedCells = 0, farObjectBlocks = 0,
        farObjectPreparingBlocks = 0;
    bool farObjectsSettled = true;
    uint64_t sceneGeneration = 0;
    bool camera = false;
    double cameraPosition[3]{};
    // View direction (inverse view matrix z axis), to check paired runs
    // share a camera.
    double cameraForward[3]{};
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
    // The user flagged a flicker on this measured frame (flag button, F8).
    void Flag(uint64_t frame);
    void Capture(Engine::Renderers::IGameRenderer& renderer, const char* fileName = "capture.ppm");
    bool Finish();
    const std::filesystem::path& Directory() const { return m_directory; }
    // Real-speed captures (GameBenchmarkOptions::captureAsync): the frame
    // asked the renderer for an async copy under this tag; Poll takes the
    // finished copies (drain: waits for all, at the end) and a writer thread
    // stores them at half resolution, so the frame loop never waits.
    void QueueAsyncCapture(uint64_t tag, std::string fileName);
    void PollAsyncCaptures(Engine::Renderers::IGameRenderer& renderer, bool drain);
    ~GameBenchmark();

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
    struct PendingImage { std::filesystem::path path; uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };
    std::map<uint64_t, std::string> m_asyncNames;
    std::deque<PendingImage> m_writeQueue;
    std::mutex m_writeMutex;
    std::condition_variable m_writeWake, m_writeRoom;
    std::thread m_writer;
    bool m_writerStop = false;
    int m_writing = 0;
    uint64_t m_asyncWritten = 0, m_asyncDropped = 0;
    void WriterLoop();
};
}
