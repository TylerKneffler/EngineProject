#pragma once
#include "pch.h"
#include "View/IEditorPanel.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <array>
#include <chrono>
#include <deque>

namespace Engine::Editor
{
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
    PerformanceView();
    ~PerformanceView() override;
    void DrawPanel(IEditorUi& ui) override;

private:
    class Sampler;
    void Refresh(float frameRate);
    void DrawOverview(IEditorUi& ui, float frameRate);
    void DrawCoreGrid(IEditorUi& ui);
    void DrawHardware(IEditorUi& ui);
    void DrawMetric(IEditorUi& ui, const char* id, const char* title,
        const EditorUiPercentageSegment* segments, size_t count,
        const std::deque<float>& history, EditorUiColor historyColor);

    std::unique_ptr<Sampler> m_sampler;
    PerformanceSnapshot m_snapshot;
    PerformanceHardware m_hardware;
    std::chrono::steady_clock::time_point m_nextSample{};
    std::deque<float> m_cpuHistory;
    std::deque<float> m_memoryHistory;
    std::deque<float> m_gpuHistory;
    std::deque<float> m_gpuMemoryHistory;
    std::deque<float> m_frameHistory;
    int m_visualMode = 0;
};
}
