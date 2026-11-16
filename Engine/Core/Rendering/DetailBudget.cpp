#include "Core/Rendering/DetailBudget.h"

#include <algorithm>
#include <cmath>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace Engine::Rendering
{
namespace
{
struct VideoMemory { uint64_t budget = 0, usage = 0; };

// Local video memory the OS grants this process, and what it uses now, on
// the high-performance adapter (the one the renderers pick). Works for every
// backend: WDDM accounts D3D and Vulkan allocations alike.
VideoMemory QueryVideoMemory()
{
    static Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter = [] {
        Microsoft::WRL::ComPtr<IDXGIAdapter3> result;
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
        for (UINT index = 0; ; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            candidate.As(&result);
            break;
        }
        return result;
    }();
    VideoMemory memory;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        memory = { info.Budget, info.CurrentUsage };
    return memory;
}
}

DetailBudget& DetailBudget::Get()
{
    static DetailBudget budget;
    return budget;
}

DetailBudget::DetailBudget()
{
    Configure(Engine::Model::DefaultProjectLayers(), {});
}

void DetailBudget::Configure(const std::vector<Engine::Model::ProjectLayer>& layers,
    const Engine::Model::DetailBudgetSettings& settings)
{
    {
        std::lock_guard lock(m_mutex);
        m_layers = layers;
        if (m_layers.empty())
            m_layers.push_back({ "Default", 1.f, 1.f });
        if (m_layers.size() > Engine::Model::MaxProjectLayers)
            m_layers.resize(Engine::Model::MaxProjectLayers);
        m_settings = settings;
        m_settings.geometryBudgetMs = std::max(.1f, m_settings.geometryBudgetMs);
        m_settings.cpuRenderBudgetMs = std::max(.1f, m_settings.cpuRenderBudgetMs);
        m_settings.minimumScale = std::clamp(m_settings.minimumScale, .05f, 1.f);
        m_settings.maximumScale = std::clamp(m_settings.maximumScale, 1.f, 20.f);
        m_settings.responseSeconds = std::max(.05f, m_settings.responseSeconds);
        m_fixed = false;
        m_settings.textureBudgetMiB = std::clamp(m_settings.textureBudgetMiB, 64.f, 65536.f);
        m_settings.extendedTextureBudget = std::clamp(m_settings.extendedTextureBudget, 0.f, 1.f);
        m_lastTextureSample = {};
        m_costBudget = 0.0;
        m_enabled.store(m_settings.enabled, std::memory_order_relaxed);
        if (!m_settings.enabled)
        {
            m_globalScale.store(1.f, std::memory_order_relaxed);
            m_steppedGlobal = m_publishedGlobal = 1.f;
        }
        else
            m_globalScale.store(std::clamp(GlobalScale(), m_settings.minimumScale,
                m_settings.maximumScale), std::memory_order_relaxed);
    }
    PublishScales();
    // Layer weights may have changed even when the global scale did not.
    m_revision.fetch_add(1, std::memory_order_relaxed);
}

std::vector<Engine::Model::ProjectLayer> DetailBudget::GetLayers() const
{
    std::lock_guard lock(m_mutex);
    return m_layers;
}

Engine::Model::DetailBudgetSettings DetailBudget::GetSettings() const
{
    std::lock_guard lock(m_mutex);
    return m_settings;
}

int DetailBudget::FindLayer(std::string_view name) const
{
    std::lock_guard lock(m_mutex);
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].name == name) return static_cast<int>(i);
    return -1;
}

int DetailBudget::FindLayerOrDefault(std::string_view name) const
{
    return std::max(0, FindLayer(name));
}

std::string DetailBudget::GetLayerName(int layer) const
{
    std::lock_guard lock(m_mutex);
    return layer >= 0 && static_cast<size_t>(layer) < m_layers.size()
        ? m_layers[static_cast<size_t>(layer)].name : std::string{};
}

void DetailBudget::AddCpuRenderTime(double milliseconds)
{
    std::lock_guard lock(m_mutex);
    m_cpuAccumulatedMs += std::max(0.0, milliseconds);
}

void DetailBudget::ReportGeometryCost(uint64_t sampleId, double geometryMs, double fixedMs)
{
    double dt = 0.0;
    {
        std::lock_guard lock(m_mutex);
        if (sampleId == m_lastSampleId) return;
        // GPU samples resolve once per frame, so the CPU render time gathered
        // since the previous sample is about one frame's worth.
        const uint64_t frames = m_lastSampleId && sampleId > m_lastSampleId
            ? std::min<uint64_t>(sampleId - m_lastSampleId, 8) : 1;
        const double cpuMs = m_cpuAccumulatedMs / static_cast<double>(frames);
        m_cpuAccumulatedMs = 0.0;
        m_lastSampleId = sampleId;
        m_lastGeometryMs.store(geometryMs, std::memory_order_relaxed);
        m_lastCpuMs.store(cpuMs, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        dt = m_lastReport == std::chrono::steady_clock::time_point{}
            ? 0.0 : std::min(.25, std::chrono::duration<double>(now - m_lastReport).count());
        m_lastReport = now;
        if (!m_settings.enabled || m_fixed || dt <= 0.0 || geometryMs <= 0.0) return;
        // Cost grows roughly with the area kept at high detail, i.e. with the
        // square of the LOD distances, so the scale follows the square root
        // of the budget ratio. A dead band just under the budget holds the
        // scale steady instead of hunting around the target.
        // Single frames spike (streaming uploads, shader warm-up), so the
        // controller follows a short running average of both costs.
        const double smoothing = 1.0 - std::exp(-dt / .25);
        m_smoothedGpuMs = m_smoothedGpuMs <= 0.0 ? geometryMs
            : m_smoothedGpuMs + (geometryMs - m_smoothedGpuMs) * smoothing;
        m_smoothedCpuMs = m_smoothedCpuMs <= 0.0 ? cpuMs
            : m_smoothedCpuMs + (cpuMs - m_smoothedCpuMs) * smoothing;
        fixedMs = std::clamp(fixedMs, 0.0, geometryMs);
        m_smoothedFixedMs = m_smoothedFixedMs <= 0.0 ? fixedMs
            : m_smoothedFixedMs + (fixedMs - m_smoothedFixedMs) * smoothing;
        // The tighter budget decides: GPU geometry time or CPU recording.
        const double gpuRatio = m_settings.geometryBudgetMs / m_smoothedGpuMs;
        const double cpuRatio = m_smoothedCpuMs > 0.0
            ? m_settings.cpuRenderBudgetMs / m_smoothedCpuMs : 4.0;
        const double tightest = std::min(gpuRatio, cpuRatio);
        const bool curveFresh = m_curve.valid &&
            now - m_curveTime < std::chrono::seconds(2);
        if (curveFresh)
        {
            // Predictive mode. The cost curve says what the visible LOD'd
            // instances would cost at every scale; the budget is a cost
            // (visibility-weighted triangles) that GPU feedback calibrates.
            // The scale is the largest the budget affords for this view, so
            // a sparse view pushes detail far out at once and a dense one
            // pulls it in, without waiting for the GPU time to move.
            const float minimum = m_settings.minimumScale, maximum = m_settings.maximumScale;
            if (m_costBudget <= 0.0)
                m_costBudget = std::max(1.0, m_curve.CostAt(std::clamp(m_publishedGlobal, minimum, maximum)));
            const double exponent = dt / m_settings.responseSeconds;
            // Only the part LOD distances change scales with the cost: the
            // budget left after terrain and grass over the time of the rest.
            const double variableBudget = m_settings.geometryBudgetMs - m_smoothedFixedMs;
            const double variableMs = std::max(.05, m_smoothedGpuMs - m_smoothedFixedMs);
            const double costRatio = std::max(.05, variableBudget) / variableMs;
            if (costRatio < 1.0)
                m_costBudget *= std::pow(std::max(costRatio, .25), exponent);
            else
            {
                // Within the GPU budget, what is drawn now is affordable.
                m_costBudget = std::max(m_costBudget,
                    m_curve.CostAt(std::clamp(m_publishedGlobal, minimum, maximum)));
                // Grow only while the budget is what limits the scale. The
                // band is narrow: the measured part is all detail-dependent.
                if (costRatio >= 1.05 && m_costBudget < m_curve.CostAt(maximum))
                    m_costBudget *= std::pow(std::min(costRatio, 2.0), exponent);
            }
            // The CPU render budget only drives the reactive controller below:
            // recording cost barely depends on instanced LOD distances, and
            // letting it hold this budget pinned detail at the minimum.
            m_costBudget = std::max(m_costBudget, 1.0);
            m_modelTarget = m_curve.LargestScaleWithin(m_costBudget, minimum, maximum);
            const float current = GlobalScale();
            const float blend = static_cast<float>(1.0 - std::exp(-dt / (.5 * m_settings.responseSeconds)));
            m_globalScale.store(current + (m_modelTarget - current) * blend, std::memory_order_relaxed);
        }
        // Inside the dead band the controller holds, but the applied scale
        // still finishes gliding to its step below.
        else if (tightest < 1.0 || tightest >= 1.0 / .85)
        {
            const double ratio = std::clamp(tightest, .25, 4.);
            const float current = GlobalScale();
            const float target = std::clamp(static_cast<float>(current * std::sqrt(ratio)),
                m_settings.minimumScale, m_settings.maximumScale);
            const float blend = static_cast<float>(1.0 - std::exp(-dt / m_settings.responseSeconds));
            m_globalScale.store(current + (target - current) * blend, std::memory_order_relaxed);
        }
    }
    PublishScales(dt);
}

void DetailBudget::SetFixedGlobalScale(float scale)
{
    {
        std::lock_guard lock(m_mutex);
        m_fixed = true;
        m_globalScale.store(std::clamp(scale, .05f, 10.f), std::memory_order_relaxed);
        m_steppedGlobal = m_publishedGlobal = GlobalScale();
    }
    PublishScales();
}

void DetailBudget::SubmitCostCurve(const DetailCostCurve& curve)
{
    std::lock_guard lock(m_mutex);
    m_curve = curve;
    m_curveTime = std::chrono::steady_clock::now();
}

DetailBudget::ModelStatus DetailBudget::GetModelStatus() const
{
    std::lock_guard lock(m_mutex);
    ModelStatus status;
    status.active = m_curve.valid && m_settings.enabled && !m_fixed &&
        std::chrono::steady_clock::now() - m_curveTime < std::chrono::seconds(2);
    status.costBudget = m_costBudget;
    status.predictedCost = m_curve.valid ? m_curve.CostAt(m_publishedGlobal) : 0.0;
    status.placements = m_curve.placements;
    status.occludedFraction = m_curve.occludedFraction;
    status.targetScale = m_modelTarget;
    return status;
}

uint64_t DetailBudget::UpdateTextureBudget(uint64_t textureBytes)
{
    std::lock_guard lock(m_mutex);
    m_texture.textureBytes = textureBytes;
    if (!m_settings.textureBudgetEnabled)
    {
        m_texture.budgetBytes = m_texture.baseBytes = m_texture.extendedBytes = 0;
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    // Configure clears the sample time, so new settings apply at once.
    const bool settingsChanged = m_lastTextureSample == std::chrono::steady_clock::time_point{};
    if (m_texture.budgetBytes && !settingsChanged &&
        now - m_lastTextureSample < std::chrono::seconds(1))
        return m_texture.budgetBytes;
    m_lastTextureSample = now;
    constexpr uint64_t MiB = 1ull << 20;
    const uint64_t base = static_cast<uint64_t>(m_settings.textureBudgetMiB) * MiB;
    const VideoMemory memory = QueryVideoMemory();
    m_texture.vramBudgetBytes = memory.budget;
    m_texture.vramUsageBytes = memory.usage;
    // Spare memory: the OS grant minus everything that is not texture
    // (buffers, render targets, terrain, staging) and a reserve for spikes.
    // Textures are excluded from the usage, so the pool does not shrink just
    // because it filled up.
    const uint64_t reserve = std::max<uint64_t>(512 * MiB, memory.budget / 10);
    const uint64_t others = memory.usage > textureBytes ? memory.usage - textureBytes : 0;
    const uint64_t spare = memory.budget > others + reserve ? memory.budget - others - reserve : 0;
    uint64_t extended = spare > base
        ? static_cast<uint64_t>(static_cast<double>(spare - base) * m_settings.extendedTextureBudget) : 0;
    extended -= extended % (64 * MiB);
    // The extended share follows other memory use loosely: small changes are
    // ignored, so the pool does not evict and reload mips back and forth.
    if (!settingsChanged && m_texture.budgetBytes && m_texture.baseBytes == base &&
        (extended > m_texture.extendedBytes ? extended - m_texture.extendedBytes
            : m_texture.extendedBytes - extended) < 256 * MiB)
        return m_texture.budgetBytes;
    m_texture.baseBytes = base;
    m_texture.extendedBytes = extended;
    m_texture.budgetBytes = base + extended;
    return m_texture.budgetBytes;
}

DetailBudget::TextureBudgetStatus DetailBudget::GetTextureBudgetStatus() const
{
    std::lock_guard lock(m_mutex);
    return m_texture;
}

void DetailBudget::PublishScales(double seconds)
{
    std::lock_guard lock(m_mutex);
    // The controller output is quantized to steps of about 12%, switched only
    // once it is well past the midpoint to the next step, so it does not
    // hunt. The applied scale then glides toward the step at a bounded rate:
    // a jump would move every LOD switch distance at once and pop a whole
    // ring of objects together. Gliding moves the switch distances slower
    // than the camera usually does, so objects cross them one at a time
    // (instanced LODs dissolve per placement, see Object.hlsl).
    constexpr float step = 1.12f;
    // Log-scale units per second: detail returns at about 12% a second and is
    // shed at about 25%, so an over-budget frame rate recovers sooner. At a
    // 60 m switch that moves the switch ring 7-15 m/s, about the camera's own
    // speed, so trees still cross it one at a time (and dissolve per tree).
    constexpr double raisePerSecond = .12, lowerPerSecond = .25;
    const float logStep = std::log(step);
    const float global = GlobalScale();
    const float offset = std::log(global) - std::log(m_steppedGlobal);
    if (std::abs(offset) > .75f * logStep)
    {
        m_steppedGlobal = std::pow(step, std::round(std::log(global) / logStep));
        m_revision.fetch_add(1, std::memory_order_relaxed);
    }
    if (seconds <= 0.0)
        m_publishedGlobal = m_steppedGlobal;
    else
    {
        const double remaining = std::log(m_steppedGlobal) - std::log(m_publishedGlobal);
        const double move = std::clamp(remaining, -lowerPerSecond * seconds, raisePerSecond * seconds);
        m_publishedGlobal = static_cast<float>(m_publishedGlobal * std::exp(move));
    }
    const float applied = m_publishedGlobal;
    for (size_t i = 0; i < m_layerScales.size(); ++i)
    {
        const auto& layer = i < m_layers.size() ? m_layers[i] : m_layers.front();
        const float scale = std::max(0.f, layer.lodDistanceScale) *
            std::pow(applied, std::max(0.f, layer.budgetWeight));
        m_layerScales[i].store(std::clamp(scale, .01f, 100.f), std::memory_order_relaxed);
    }
}
}
