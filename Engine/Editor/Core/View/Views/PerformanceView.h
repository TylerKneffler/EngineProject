#pragma once
#include "pch.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <array>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Engine::Scene { class Scene; }
namespace Engine::Renderers { class IEditorRenderer; }

namespace Engine::Editor
{
struct EditorFrameProfileSample;
class PerformanceRecording;
struct PerformanceCoreLoad
{
    float user = 0.f;
    float kernel = 0.f;
};

struct PerformanceSnapshot
{
    float cpuUser = 0.f;
    float cpuKernel = 0.f;
    std::vector<PerformanceCoreLoad> cores;
    uint64_t memoryTotal = 0;
    uint64_t memoryUsed = 0;
    uint64_t processMemory = 0;
    uint64_t systemCache = 0;
    uint64_t commitUsed = 0;
    uint64_t commitLimit = 0;
    std::array<float, 5> gpuEngines{};
    uint64_t gpuMemoryUsed = 0;
    uint64_t gpuMemoryBudget = 0;
};

struct PerformanceHardware
{
    std::string cpuName = "Unknown CPU";
    std::string gpuName = "Unknown GPU";
    uint32_t physicalCores = 0;
    uint32_t logicalCores = 0;
    uint32_t cpuMHz = 0;
    std::array<uint64_t, 4> cacheBytes{};
    uint64_t installedMemory = 0;
    uint64_t dedicatedVideoMemory = 0;
    uint64_t sharedVideoMemory = 0;
};

class PerformanceView final : public IEditorPanel
{
public:
    PerformanceView(Engine::Scene::Scene* scene,
        std::shared_ptr<PerformanceRecording> recording = {});
    ~PerformanceView() override;
    void DrawPanel(IEditorUi& ui) override;
    void SetFrameTimings(const EditorFrameProfileSample& sample,
        Renderers::IEditorRenderer* renderer);

private:
    class Sampler;
    void Refresh(float frameRate);
    void DrawOverview(IEditorUi& ui, float frameRate);
    void DrawCoreGrid(IEditorUi& ui);
    void DrawHardware(IEditorUi& ui);
    void DrawShadows(IEditorUi& ui);
    void DrawTimings(IEditorUi& ui);
    void DrawRecording(IEditorUi& ui);
    void RecordSnapshot(float frameRate);
    void StartRecording();
    void StopRecording();
    void DrawMetric(IEditorUi& ui, const char* id, const char* title,
        const EditorUiPercentageSegment* segments, size_t count,
        const std::deque<float>& history, EditorUiColor historyColor);

    std::thread m_samplerWorker;
    std::mutex m_samplerMutex;
    std::condition_variable m_samplerWake;
    bool m_stopSampler = false;
    bool m_samplerActive = true;
    uint64_t m_samplerVersion = 0;
    uint64_t m_appliedSamplerVersion = 0;
    PerformanceSnapshot m_pendingSnapshot;
    PerformanceHardware m_pendingHardware;
    PerformanceSnapshot m_snapshot;
    PerformanceHardware m_hardware;
    std::chrono::steady_clock::time_point m_lastFrame{};
    std::deque<double> m_recentFrameMs;
    double m_recentFrameTotalMs = 0.0;
    float m_displayFrameRate = 0.f;
    double m_lastPanelDrawMs = 0.0;
    double m_lastCounterSampleMs = 0.0;
    double m_pendingCounterSampleMs = 0.0;
    std::deque<float> m_cpuHistory;
    std::deque<float> m_memoryHistory;
    std::deque<float> m_gpuHistory;
    std::deque<float> m_gpuMemoryHistory;
    std::deque<float> m_frameHistory;
    int m_visualMode = 0;
    Engine::Scene::Scene* m_scene = nullptr;
    std::shared_ptr<PerformanceRecording> m_recording;
    std::string m_recordingPath;
    std::string m_recordingError;
    struct TimingValue { const char* label; double milliseconds; };
    std::vector<TimingValue> m_frameTimings;
    std::vector<TimingValue> m_updateTimings;
    std::vector<TimingValue> m_renderTimings;
    std::vector<TimingValue> m_gpuQueryTimings;
    std::vector<TimingValue> m_gpuTimings;
    bool m_gpuTimingsValid = false;
    bool m_hasFrameTimings = false;
};
}
