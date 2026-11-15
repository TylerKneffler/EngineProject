#include "Core/Rendering/DetailBudget.h"

#include <algorithm>
#include <cmath>

namespace Engine::Rendering
{
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
        m_settings.maximumScale = std::clamp(m_settings.maximumScale, 1.f, 10.f);
        m_settings.responseSeconds = std::max(.05f, m_settings.responseSeconds);
        m_fixed = false;
        m_enabled.store(m_settings.enabled, std::memory_order_relaxed);
        if (!m_settings.enabled)
        {
            m_globalScale.store(1.f, std::memory_order_relaxed);
            m_publishedGlobal = 1.f;
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

void DetailBudget::ReportGeometryCost(uint64_t sampleId, double geometryMs)
{
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
        const double dt = m_lastReport == std::chrono::steady_clock::time_point{}
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
        // The tighter budget decides: GPU geometry time or CPU recording.
        const double gpuRatio = m_settings.geometryBudgetMs / m_smoothedGpuMs;
        const double cpuRatio = m_smoothedCpuMs > 0.0
            ? m_settings.cpuRenderBudgetMs / m_smoothedCpuMs : 4.0;
        const double tightest = std::min(gpuRatio, cpuRatio);
        if (tightest >= 1.0 && tightest < 1.0 / .85) return;
        const double ratio = std::clamp(tightest, .25, 4.);
        const float current = GlobalScale();
        const float target = std::clamp(static_cast<float>(current * std::sqrt(ratio)),
            m_settings.minimumScale, m_settings.maximumScale);
        const float blend = static_cast<float>(1.0 - std::exp(-dt / m_settings.responseSeconds));
        m_globalScale.store(current + (target - current) * blend, std::memory_order_relaxed);
    }
    PublishScales();
}

void DetailBudget::SetFixedGlobalScale(float scale)
{
    {
        std::lock_guard lock(m_mutex);
        m_fixed = true;
        m_globalScale.store(std::clamp(scale, .05f, 10.f), std::memory_order_relaxed);
        m_publishedGlobal = GlobalScale();
    }
    PublishScales();
}

void DetailBudget::PublishScales()
{
    std::lock_guard lock(m_mutex);
    // LOD distances follow a stepped copy of the eased global scale: steps
    // of about 12%, switched only once the eased value is well past the
    // midpoint to the next step. A continuously drifting scale would slide
    // every LOD boundary each frame, flipping objects near a boundary back
    // and forth (visible flicker).
    constexpr float step = 1.12f;
    const float logStep = std::log(step);
    const float global = GlobalScale();
    const float offset = std::log(global) - std::log(m_publishedGlobal);
    if (std::abs(offset) > .75f * logStep)
    {
        m_publishedGlobal = std::pow(step, std::round(std::log(global) / logStep));
        m_revision.fetch_add(1, std::memory_order_relaxed);
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
