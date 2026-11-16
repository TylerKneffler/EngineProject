#include "Core/Rendering/HardwareSimulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Engine::Rendering
{
HardwareSimulation& HardwareSimulation::Get()
{
    static HardwareSimulation instance;
    return instance;
}

void HardwareSimulation::Configure(float cpuLoadMs, float gpuLoadMs)
{
    m_cpuLoadMs.store(std::clamp(cpuLoadMs, 0.f, 200.f), std::memory_order_relaxed);
    m_gpuLoadMs.store(std::clamp(gpuLoadMs, 0.f, 200.f), std::memory_order_relaxed);
    m_iterations.store(InitialIterations, std::memory_order_relaxed);
    m_lastSample.store(0, std::memory_order_relaxed);
    m_lastGpuMs.store(0.0, std::memory_order_relaxed);
}

void HardwareSimulation::SpendCpu() const
{
    const float milliseconds = CpuLoadMs();
    if (milliseconds <= 0.f) return;
    // Busy, not sleeping: a slower CPU is busy for longer, and a sleep would
    // free the core for other threads.
    const auto end = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(milliseconds));
    while (std::chrono::steady_clock::now() < end) {}
}

void HardwareSimulation::ReportGpuLoad(uint64_t sampleId, double measuredMs)
{
    const float target = GpuLoadMs();
    if (target <= 0.f || sampleId == 0 || sampleId == m_lastSample.load(std::memory_order_relaxed)) return;
    m_lastSample.store(sampleId, std::memory_order_relaxed);
    m_lastGpuMs.store(measuredMs, std::memory_order_relaxed);
    // Time scales about linearly with iterations; step toward the ratio,
    // limited so one odd sample cannot swing the load far.
    const double ratio = measuredMs > .01 ? std::clamp(target / measuredMs, .5, 2.) : 2.;
    const double next = std::clamp(GpuIterations() * std::pow(ratio, .7),
        static_cast<double>(MinimumIterations), static_cast<double>(MaximumIterations));
    m_iterations.store(static_cast<uint32_t>(next), std::memory_order_relaxed);
}
}
