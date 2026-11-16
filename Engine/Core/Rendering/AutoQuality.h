#pragma once

#include "Core/Model/GraphicsQualitySettings.h"

#include <array>
#include <mutex>

namespace Engine::Rendering
{
// Auto graphics quality (GraphicsQualitySettings::AutoPreset).
//
// Holds a frame-time target with the best quality the hardware sustains.
// Each frame it is given the frame's work time (the frame interval without
// the frame-rate limiter's sleep, so it covers CPU- and GPU-bound frames).
// Over the target it lowers one quality channel; with clear headroom it
// raises one back. It moves one step at a time and waits for the frame time
// to settle in between, so every step's effect can be measured.
//
// Choosing the channel is what gives the best performance-to-quality ratio.
// Each channel carries:
// - importance: how visible a unit of it is (texture blur and flat shading
//   show more than a shorter grass radius), fixed.
// - cost: milliseconds per unit of the channel, learned. It starts from a
//   prior and is updated from the frame-time change every step causes, so
//   the controller learns what actually costs time on this machine and
//   scene.
// Lowering picks the channel that saves the most time per unit of
// importance; raising picks the one that buys the most importance per
// millisecond, and only when its learned cost fits under the target.
//
// LOD distances are not a channel here: the detail budget (DetailBudget.h)
// owns them. While Auto is active its GPU geometry budget is a share of the
// frame-time target instead of the project's fixed milliseconds.
class AutoQualityController
{
public:
    // Order of GraphicsQualitySettings::Channels.
    enum Channel { Geometry, Vegetation, ViewDistance, Texture, Material, Shadows, RenderScale, ChannelCount };

    struct ChannelPolicy
    {
        bool adjustable = false;
        float minimum = 1.f, step = .1f;
        float importance = 1.f;
        // Prior milliseconds saved per unit of the channel (1 -> 0).
        float priorMsPerUnit = 1.f;
    };
    static std::array<ChannelPolicy, ChannelCount> DefaultPolicies();

    // Frame time is held between these fractions of the target.
    static constexpr double OverFraction = 1.03;
    static constexpr double HeadroomFraction = .85;
    // A raise must be predicted to keep the frame under this fraction.
    static constexpr double RaiseLimitFraction = .95;
    static constexpr double SettleSeconds = .75;
    static constexpr double RaiseCooldownSeconds = 3.0;
    static constexpr double SmoothingSeconds = .4;
    // An overload or headroom must last this long before a step, so single
    // spikes and dips never move quality.
    static constexpr double OverloadSeconds = .5;
    static constexpr double HeadroomSeconds = 1.0;

    explicit AutoQualityController(const std::array<ChannelPolicy, ChannelCount>& policies = DefaultPolicies());

    // frameMs: this frame's work time; targetMs: the frame-time target;
    // seconds: time since the previous call. Returns true when a channel
    // changed.
    bool Update(double frameMs, double targetMs, double seconds);
    void Reset();

    const Engine::Model::GraphicsQualityValues& Values() const { return m_values; }
    float Value(Channel channel) const { return *Channels()[channel]; }
    double SmoothedFrameMs() const { return m_smoothedMs; }
    double LearnedMsPerUnit(Channel channel) const { return m_msPerUnit[channel]; }
    int LastChangedChannel() const { return m_lastChanged; }

private:
    std::array<float*, ChannelCount> Channels();
    std::array<const float*, ChannelCount> Channels() const;
    void Apply(int channel, float value);

    std::array<ChannelPolicy, ChannelCount> m_policies;
    Engine::Model::GraphicsQualityValues m_values;
    std::array<double, ChannelCount> m_msPerUnit{};
    double m_smoothedMs = 0.0;
    double m_sinceChange = 0.0;
    double m_raiseCooldown = 0.0;
    double m_overSeconds = 0.0, m_headroomSeconds = 0.0;
    // The step being measured: its channel, size and the frame time before.
    int m_pendingChannel = -1;
    float m_pendingDelta = 0.f;
    double m_pendingBeforeMs = 0.0;
    int m_lastChanged = -1;
};

// The game's controller: publishes its values to the Auto preset and sets
// the detail budget's geometry share while Auto is active.
class AutoQuality
{
public:
    static AutoQuality& Get();

    // Share of the frame-time target given to GPU geometry passes (the rest
    // covers lighting, post and presentation, which are not measured).
    static constexpr float GeometryBudgetShare = .6f;

    // Call once per frame. active: the scene uses the Auto preset.
    void Update(bool active, double frameMs, double targetMs, double seconds);

    struct Status
    {
        bool active = false;
        double targetMs = 0.0, frameMs = 0.0;
        Engine::Model::GraphicsQualityValues values;
    };
    Status GetStatus() const;

private:
    mutable std::mutex m_mutex;
    AutoQualityController m_controller;
    bool m_active = false;
    double m_targetMs = 0.0;
    float m_projectGeometryBudgetMs = 0.f;
};
}
