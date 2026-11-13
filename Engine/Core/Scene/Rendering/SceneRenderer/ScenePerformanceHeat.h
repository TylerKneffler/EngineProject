#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Scene::Renderer
{
// Editor "Performance" scene render mode: shades every draw from blue (cheap)
// to red (expensive) by how much CPU and GPU work it costs.
//
// CPU cost is measured: the time this frame spent preparing the draw (material
// resolution, lighting policy, pipeline selection, geometry binding).
//
// GPU cost is estimated from the work the draw submits, because per-draw GPU
// timestamps are not reliable on tiled/overlapping hardware and the heat view
// itself replaces the material shader it would be measuring:
//   vertex work  = vertices (or indices) x instances x vertex-shader weight
//                  (skinning, morph targets, wind)
//   pixel work   = projected screen pixels x pixel-shader weight
//                  (texture fetches, lights, shadow filtering, parallax,
//                  subsurface, reflections, blending)
//   shadow work  = vertex work repeated for every shadow cascade it casts into
// Both terms are converted to microseconds with mid-range-GPU throughput
// constants, so CPU and GPU costs can be summed and compared.
struct PerformanceHeatDrawInput
{
    uint64_t key = 0;               // object + mesh identity, for smoothing
    const std::string* name = nullptr;
    double cpuMicroseconds = 0.0;
    uint64_t vertexElements = 0;
    float vertexShaderWeight = 1.f;
    float projectedPixels = 0.f;
    float pixelShaderWeight = 1.f;
    uint64_t shadowElements = 0;
};

struct PerformanceHeatEntry
{
    std::string name;
    float totalMicroseconds = 0.f;
    float cpuMicroseconds = 0.f;
    float gpuMicroseconds = 0.f;
};

struct PerformanceHeatSnapshot
{
    std::vector<PerformanceHeatEntry> hottest; // most expensive first
    float totalMicroseconds = 0.f;
    uint32_t drawCount = 0;
};

class PerformanceHeatMap
{
public:
    // Writes one heat value in [0, 1] per input, in input order. Costs are
    // smoothed over frames, then mapped on a log scale from the frame's cheap
    // end (10th percentile, blue) to its most expensive draw (red).
    void Resolve(const std::vector<PerformanceHeatDrawInput>& inputs,
        std::vector<float>& heat);
    PerformanceHeatSnapshot GetSnapshot() const;

    static uint64_t MakeKey(const void* object, const void* mesh);
    // Screen pixels covered by the projected world-space bounds (a rectangle
    // estimate, halved to approximate the silhouette inside it).
    static float ProjectedPixels(const glm::vec3& boundsMin,
        const glm::vec3& boundsMax, const glm::mat4& world,
        const glm::mat4& viewProjection, uint32_t viewportWidth,
        uint32_t viewportHeight);

private:
    struct History
    {
        float cpuMicroseconds = 0.f;
        float gpuMicroseconds = 0.f;
        uint64_t lastResolve = 0;
    };
    std::unordered_map<uint64_t, History> m_history;
    uint64_t m_resolveCount = 0;
    mutable std::mutex m_snapshotMutex;
    PerformanceHeatSnapshot m_snapshot;
};
}
