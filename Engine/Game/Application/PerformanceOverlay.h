#pragma once

#include "Core/Scene/Scene.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace Engine::Game
{
// Live performance overlay in the top right of the standalone game: a line
// graph of frame time, GPU time, main-thread CPU time and update time, with
// guide lines at 100 / 80 / 60 / 30 fps, and a text readout of fps, the GPU
// stages and the CPU stages. Its UI is ordinary runtime-only scene objects
// (Canvas, UIObject, UIImage, UIText, UILineGraph), so the same graph can be
// built in the editor. Shown when the project setting
// `showPerformanceOverlay` is on; F3 toggles it.
class PerformanceOverlay
{
public:
    struct Sample
    {
        double frameMs = 0.0;          // interval since the last frame
        double gpuMs = 0.0;            // sum of the timed GPU stages
        double gpuTerrainMs = 0.0, gpuGrassMs = 0.0, gpuCullMs = 0.0, gpuOpaqueMs = 0.0;
        double gpuShadowMs = 0.0, gpuSkyCloudsMs = 0.0, gpuImpostorMs = 0.0;
        double updateMs = 0.0, prepareMs = 0.0, cameraRecordMs = 0.0, shadowRecordMs = 0.0;
        double renderThreadMs = 0.0;
        uint32_t width = 0, height = 0;
        bool gpuValid = false;
    };

    void SetEnabled(bool enabled) { m_enabled.store(enabled); }
    bool IsEnabled() const { return m_enabled.load(); }
    void Toggle() { m_enabled.store(!m_enabled.load()); }
    // Canvas units to leave free above the panel (the flicker flag button).
    void SetTopOffset(float offset) { m_topOffset = offset; }

    // Render thread: one sample per presented frame.
    void Push(const Sample& sample);
    // Simulation thread, during the update: builds the UI the first time
    // (and after a scene load removed it), feeds the graph, refreshes text.
    void Update(Engine::Scene::Scene& scene);

private:
    std::atomic<bool> m_enabled{ false };
    std::mutex m_mutex;
    std::vector<Sample> m_pending;
    std::deque<Sample> m_window;   // the graph's span of samples, for statistics
    Engine::Scene::Scene::ObjectId m_rootId{};
    Engine::Scene::Scene::ObjectId m_graphId{};
    // Value texts: header (fps, frame line), legend values, metric cells.
    std::vector<Engine::Scene::Scene::ObjectId> m_valueIds;
    double m_textAgeMs = 1.e9;
    float m_topOffset = 0.f;
    bool m_shown = false;
};
}
