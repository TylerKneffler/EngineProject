#include "pch.h"
#include "Core/Rendering/VisibilityMonitor.h"
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace Engine::Rendering
{
// The GPU list: record 0 holds the indirect arguments (row0.y = kept count),
// record i + 1 the i-th kept placement (row0.y = its pool index; see
// InstanceCull.hlsl). Compared with the CPU's expected pool indices.
void PersistentInstanceBatches::CompleteMonitorReadback(Batch& batch, const uint8_t* mapped)
{
    auto& monitor = batch.monitor;
    constexpr size_t record = sizeof(VisibleInstanceRecord);
    uint32_t kept = 0;
    std::memcpy(&kept, mapped + sizeof(uint32_t), sizeof(kept));
    const uint32_t capacity = monitor.bytes >= record ? uint32_t(monitor.bytes / record) - 1u : 0u;
    kept = std::min(kept, capacity);
    std::unordered_set<uint32_t> gpu;
    gpu.reserve(kept);
    for (uint32_t i = 0; i < kept; ++i)
    {
        uint32_t index = 0;
        std::memcpy(&index, mapped + (size_t(i) + 1) * record + sizeof(uint32_t), sizeof(index));
        gpu.insert(index);
    }
    uint32_t missing = 0;
    std::ostringstream samples;
    std::unordered_set<uint32_t> expected(monitor.expected.begin(), monitor.expected.end());
    for (size_t i = 0; i < monitor.expected.size(); ++i)
    {
        if (gpu.count(monitor.expected[i])) continue;
        if (missing < 8)
        {
            const auto& info = monitor.info[i];
            samples << (missing ? "," : "") << "{\"pool\":" << monitor.expected[i] << ",\"tile\":\""
                << (i < monitor.tileNames.size() ? monitor.tileNames[i] : std::string()) << "\",\"pos\":["
                << info.x << "," << info.y << "," << info.z << "],\"distance\":" << info.w << "}";
        }
        ++missing;
    }
    uint32_t extra = 0;
    for (const uint32_t index : gpu) extra += expected.count(index) ? 0u : 1u;
    VisibilityMonitor::Get().ReportBatch(monitor.serial, monitor.label,
        uint32_t(monitor.expected.size()), kept, missing, extra, samples.str());
}
}
