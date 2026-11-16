#include "Core/Rendering/AutoQuality.h"

#include "Core/Rendering/DetailBudget.h"

#include <algorithm>
#include <cmath>

namespace Engine::Rendering
{
std::array<AutoQualityController::ChannelPolicy, AutoQualityController::ChannelCount>
AutoQualityController::DefaultPolicies()
{
    std::array<ChannelPolicy, ChannelCount> policies{};
    // Geometry (LOD distances) belongs to the detail budget. Render scale is
    // not applied by the standalone game, so lowering it would cost quality
    // in the editor views and save nothing in the game.
    policies[Geometry] = {};
    policies[RenderScale] = {};
    // Grass and foliage draw range: cheap to lose at the edge of the field.
    policies[Vegetation] = { true, .4f, .1f, 1.f, 3.f };
    // Far-plane culling: distant objects pop, but cost little each.
    policies[ViewDistance] = { true, .5f, .1f, 2.f, 3.f };
    // Mip bias: blur is plainly visible; saves bandwidth more than time.
    policies[Texture] = { true, .4f, .1f, 3.f, 1.f };
    // Any value below 1 switches to simplified shading, so it is one step.
    policies[Material] = { true, .5f, .5f, 2.5f, 2.f };
    // Shadow distance and cascade count.
    policies[Shadows] = { true, .4f, .1f, 1.5f, 4.f };
    return policies;
}

AutoQualityController::AutoQualityController(const std::array<ChannelPolicy, ChannelCount>& policies)
    : m_policies(policies)
{
    Reset();
}

void AutoQualityController::Reset()
{
    m_values = {};
    for (size_t channel = 0; channel < ChannelCount; ++channel)
        m_msPerUnit[channel] = std::max(.05, static_cast<double>(m_policies[channel].priorMsPerUnit));
    m_smoothedMs = 0.0;
    m_sinceChange = 0.0;
    m_raiseCooldown = 0.0;
    m_overSeconds = m_headroomSeconds = 0.0;
    m_pendingChannel = -1;
    m_lastChanged = -1;
}

std::array<float*, AutoQualityController::ChannelCount> AutoQualityController::Channels()
{
    const auto channels = Engine::Model::GraphicsQualitySettings::Channels(m_values);
    std::array<float*, ChannelCount> result{};
    std::copy(channels.begin(), channels.end(), result.begin());
    return result;
}

std::array<const float*, AutoQualityController::ChannelCount> AutoQualityController::Channels() const
{
    auto copy = const_cast<AutoQualityController*>(this)->Channels();
    std::array<const float*, ChannelCount> result{};
    std::copy(copy.begin(), copy.end(), result.begin());
    return result;
}

void AutoQualityController::Apply(int channel, float value)
{
    float& current = *Channels()[channel];
    m_pendingChannel = channel;
    m_pendingDelta = value - current;
    m_pendingBeforeMs = m_smoothedMs;
    current = value;
    m_sinceChange = 0.0;
    m_overSeconds = m_headroomSeconds = 0.0;
    m_lastChanged = channel;
}

bool AutoQualityController::Update(double frameMs, double targetMs, double seconds)
{
    if (!(frameMs > 0.0) || !(targetMs > 0.0) || !std::isfinite(frameMs)) return false;
    seconds = std::clamp(seconds, 0.0, .25);
    // A short running average: single frames spike (streaming, shader
    // warm-up) and must not move quality on their own.
    if (m_smoothedMs <= 0.0) m_smoothedMs = frameMs;
    else m_smoothedMs += (frameMs - m_smoothedMs) * (1.0 - std::exp(-seconds / SmoothingSeconds));
    m_sinceChange += seconds;
    m_raiseCooldown = std::max(0.0, m_raiseCooldown - seconds);
    m_overSeconds = m_smoothedMs > targetMs * OverFraction ? m_overSeconds + seconds : 0.0;
    m_headroomSeconds = m_smoothedMs < targetMs * HeadroomFraction ? m_headroomSeconds + seconds : 0.0;
    if (m_sinceChange < SettleSeconds) return false;

    // Learn what the last step did: milliseconds per unit of the channel.
    if (m_pendingChannel >= 0 && std::abs(m_pendingDelta) > 1.e-4f)
    {
        // Lowering saves time (before > after); raising costs it.
        const double change = (m_pendingBeforeMs - m_smoothedMs) * (m_pendingDelta < 0.f ? 1.0 : -1.0);
        const double observed = std::clamp(change / std::abs(m_pendingDelta), .05, 50.0);
        m_msPerUnit[m_pendingChannel] += (observed - m_msPerUnit[m_pendingChannel]) * .5;
        m_pendingChannel = -1;
    }

    const auto values = Channels();
    if (m_overSeconds >= OverloadSeconds)
    {
        // Most time saved per unit of visible quality lost.
        int best = -1;
        double bestScore = 0.0;
        for (int channel = 0; channel < ChannelCount; ++channel)
        {
            const auto& policy = m_policies[channel];
            if (!policy.adjustable || *values[channel] <= policy.minimum + 1.e-4f) continue;
            const double score = m_msPerUnit[channel] / policy.importance;
            if (score > bestScore) { bestScore = score; best = channel; }
        }
        if (best < 0) return false;
        Apply(best, std::max(m_policies[best].minimum, *values[best] - m_policies[best].step));
        // Do not climb straight back into the same overload.
        m_raiseCooldown = RaiseCooldownSeconds;
        return true;
    }
    if (m_headroomSeconds >= HeadroomSeconds && m_raiseCooldown <= 0.0)
    {
        // Most visible quality per millisecond, if it fits.
        int best = -1;
        double bestScore = 0.0;
        for (int channel = 0; channel < ChannelCount; ++channel)
        {
            const auto& policy = m_policies[channel];
            if (!policy.adjustable || *values[channel] >= 1.f - 1.e-4f) continue;
            const float raised = std::min(1.f, *values[channel] + policy.step);
            const double cost = m_msPerUnit[channel] * (raised - *values[channel]);
            if (m_smoothedMs + cost > targetMs * RaiseLimitFraction) continue;
            const double score = policy.importance / m_msPerUnit[channel];
            if (score > bestScore) { bestScore = score; best = channel; }
        }
        if (best < 0) return false;
        Apply(best, std::min(1.f, *values[best] + m_policies[best].step));
        return true;
    }
    return false;
}

AutoQuality& AutoQuality::Get()
{
    static AutoQuality instance;
    return instance;
}

void AutoQuality::Update(bool active, double frameMs, double targetMs, double seconds)
{
    std::lock_guard lock(m_mutex);
    auto& budget = DetailBudget::Get();
    if (active != m_active)
    {
        m_active = active;
        m_controller.Reset();
        if (active)
            m_projectGeometryBudgetMs = budget.GetSettings().geometryBudgetMs;
        else
            budget.SetGeometryBudgetMs(m_projectGeometryBudgetMs);
    }
    if (!active) return;
    m_targetMs = targetMs;
    budget.SetGeometryBudgetMs(static_cast<float>(targetMs) * GeometryBudgetShare);
    m_controller.Update(frameMs, targetMs, seconds);
    auto& shared = Engine::Model::SharedAutoQualityChannels();
    auto values = m_controller.Values();
    const auto channels = Engine::Model::GraphicsQualitySettings::Channels(values);
    for (size_t channel = 0; channel < channels.size(); ++channel)
        shared.values[channel].store(*channels[channel], std::memory_order_relaxed);
}

AutoQuality::Status AutoQuality::GetStatus() const
{
    std::lock_guard lock(m_mutex);
    Status status;
    status.active = m_active;
    status.targetMs = m_targetMs;
    status.frameMs = m_controller.SmoothedFrameMs();
    status.values = m_controller.Values();
    return status;
}
}
