#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace Engine::Rendering
{
// Makes this machine behave like a weaker one, for testing video profiles
// (Model/VideoProfiles.h). Extra CPU time is spent on the main thread each
// frame; extra GPU time comes from a compute pass (Shaders/GpuLoad) whose
// iteration count is calibrated every frame from its measured GPU time
// (GpuTimingStage::SyntheticLoad), so it holds the requested milliseconds
// on any GPU.
//
// The GPU load is a fixed cost per frame, as if a slower GPU had that much
// more work. It does not slow each pass in proportion, so the detail budget
// (which measures geometry passes) does not see it; frame time and Auto
// quality do.
class HardwareSimulation
{
public:
    static HardwareSimulation& Get();

    void Configure(float cpuLoadMs, float gpuLoadMs);
    // Name of the applied video profile, for logs; set once at startup.
    void SetProfileName(std::string name) { m_profileName = std::move(name); }
    const std::string& ProfileName() const { return m_profileName; }
    float CpuLoadMs() const { return m_cpuLoadMs.load(std::memory_order_relaxed); }
    float GpuLoadMs() const { return m_gpuLoadMs.load(std::memory_order_relaxed); }

    // Busy-waits the configured CPU milliseconds on the calling thread.
    void SpendCpu() const;

    // Iterations per thread of the load shader for the next dispatch.
    uint32_t GpuIterations() const { return m_iterations.load(std::memory_order_relaxed); }
    // One resolved frame's measured load time; repeated sample ids are
    // ignored. Moves the iteration count toward the target.
    void ReportGpuLoad(uint64_t sampleId, double measuredMs);
    double LastGpuLoadMs() const { return m_lastGpuMs.load(std::memory_order_relaxed); }

    static constexpr uint32_t InitialIterations = 2048;
    static constexpr uint32_t MinimumIterations = 16;
    static constexpr uint32_t MaximumIterations = 1u << 22;

private:
    std::atomic<float> m_cpuLoadMs{ 0.f }, m_gpuLoadMs{ 0.f };
    std::atomic<uint32_t> m_iterations{ InitialIterations };
    std::atomic<uint64_t> m_lastSample{ 0 };
    std::atomic<double> m_lastGpuMs{ 0.0 };
    std::string m_profileName;
};
}
