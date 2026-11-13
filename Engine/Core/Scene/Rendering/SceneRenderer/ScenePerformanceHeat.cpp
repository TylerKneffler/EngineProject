#include "pch.h"
#include "Core/Scene/Rendering/SceneRenderer/ScenePerformanceHeat.h"

#include <algorithm>
#include <cmath>

namespace Engine::Scene::Renderer
{
namespace
{
// Mid-range desktop GPU throughput, in microseconds per unit of work. Only the
// ratios matter for the colors; absolute values make the tooltip readable.
constexpr double kVertexMicroseconds = 0.00025;   // ~4 G vertex invocations/s
constexpr double kPixelMicroseconds = 0.0001;     // ~10 G simple pixel ops/s
constexpr double kShadowVertexMicroseconds = 0.00015; // depth-only vertices
constexpr float kSmoothing = 0.15f;               // per-resolve EMA weight
constexpr uint64_t kHistoryLifetime = 240;        // resolves before pruning
constexpr size_t kHottestCount = 8;
}

uint64_t PerformanceHeatMap::MakeKey(const void* object, const void* mesh)
{
    const uint64_t a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
    const uint64_t b = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mesh));
    return (a * 0x9E3779B97F4A7C15ull) ^ (b + 0x7F4A7C159E3779B9ull + (a << 6) + (a >> 2));
}

float PerformanceHeatMap::ProjectedPixels(const glm::vec3& boundsMin,
    const glm::vec3& boundsMax, const glm::mat4& world,
    const glm::mat4& viewProjection, uint32_t viewportWidth,
    uint32_t viewportHeight)
{
    const float viewportPixels =
        static_cast<float>(viewportWidth) * static_cast<float>(viewportHeight);
    const glm::mat4 clipFromLocal = viewProjection * world;
    glm::vec2 low(1.f), high(-1.f);
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 local(
            (corner & 1) ? boundsMax.x : boundsMin.x,
            (corner & 2) ? boundsMax.y : boundsMin.y,
            (corner & 4) ? boundsMax.z : boundsMin.z);
        const glm::vec4 clip = clipFromLocal * glm::vec4(local, 1.f);
        // Bounds straddling the camera plane can cover the whole view.
        if (clip.w <= 1.e-4f)
            return viewportPixels * 0.5f;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (corner == 0) low = high = ndc;
        low = glm::min(low, ndc);
        high = glm::max(high, ndc);
    }
    low = glm::clamp(low, glm::vec2(-1.f), glm::vec2(1.f));
    high = glm::clamp(high, glm::vec2(-1.f), glm::vec2(1.f));
    if (high.x <= low.x || high.y <= low.y)
        return 0.f;
    return (high.x - low.x) * (high.y - low.y) * 0.25f * viewportPixels * 0.5f;
}

void PerformanceHeatMap::Resolve(const std::vector<PerformanceHeatDrawInput>& inputs,
    std::vector<float>& heat)
{
    ++m_resolveCount;
    heat.assign(inputs.size(), 0.f);
    if (inputs.empty())
        return;

    std::vector<float> totals(inputs.size());
    std::vector<const History*> histories(inputs.size());
    for (size_t index = 0; index < inputs.size(); ++index)
    {
        const auto& input = inputs[index];
        const double gpu =
            static_cast<double>(input.vertexElements) * input.vertexShaderWeight *
                kVertexMicroseconds +
            static_cast<double>(input.projectedPixels) * input.pixelShaderWeight *
                kPixelMicroseconds +
            static_cast<double>(input.shadowElements) * kShadowVertexMicroseconds;
        auto [entry, inserted] = m_history.try_emplace(input.key);
        History& history = entry->second;
        // Restart smoothing for draws that were out of view for a while.
        if (inserted || history.lastResolve + 8 < m_resolveCount)
        {
            history.cpuMicroseconds = static_cast<float>(input.cpuMicroseconds);
            history.gpuMicroseconds = static_cast<float>(gpu);
        }
        else
        {
            history.cpuMicroseconds += kSmoothing *
                (static_cast<float>(input.cpuMicroseconds) - history.cpuMicroseconds);
            history.gpuMicroseconds += kSmoothing *
                (static_cast<float>(gpu) - history.gpuMicroseconds);
        }
        history.lastResolve = m_resolveCount;
        histories[index] = &history;
        totals[index] = history.cpuMicroseconds + history.gpuMicroseconds;
    }

    // Log mapping keeps the many cheap draws distinguishable while the most
    // expensive draw stays fully red. A minimum span stops a scene of equally
    // cheap draws from rendering entirely red.
    std::vector<float> sorted = totals;
    std::sort(sorted.begin(), sorted.end());
    const float low = std::max(sorted[sorted.size() / 10], 0.5f);
    const float high = std::max(sorted.back(), low * 8.f);
    const float span = std::log(high / low);
    for (size_t index = 0; index < totals.size(); ++index)
        heat[index] = std::clamp(
            std::log(std::max(totals[index], low) / low) / span, 0.f, 1.f);

    PerformanceHeatSnapshot snapshot;
    snapshot.drawCount = static_cast<uint32_t>(totals.size());
    std::vector<size_t> order(totals.size());
    for (size_t index = 0; index < order.size(); ++index)
    {
        order[index] = index;
        snapshot.totalMicroseconds += totals[index];
    }
    const size_t hottest = std::min(kHottestCount, order.size());
    std::partial_sort(order.begin(), order.begin() + hottest, order.end(),
        [&](size_t a, size_t b) { return totals[a] > totals[b]; });
    for (size_t rank = 0; rank < hottest; ++rank)
    {
        const size_t index = order[rank];
        PerformanceHeatEntry entry;
        entry.name = inputs[index].name && !inputs[index].name->empty()
            ? *inputs[index].name : std::string("(unnamed)");
        entry.cpuMicroseconds = histories[index]->cpuMicroseconds;
        entry.gpuMicroseconds = histories[index]->gpuMicroseconds;
        entry.totalMicroseconds = totals[index];
        snapshot.hottest.push_back(std::move(entry));
    }
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(snapshot);
    }

    if (m_resolveCount % kHistoryLifetime == 0)
        for (auto entry = m_history.begin(); entry != m_history.end();)
            entry = entry->second.lastResolve + kHistoryLifetime < m_resolveCount
                ? m_history.erase(entry) : std::next(entry);
}

PerformanceHeatSnapshot PerformanceHeatMap::GetSnapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}
}
