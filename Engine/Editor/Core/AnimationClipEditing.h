#pragma once

#include "Core/Model/AnimationClip.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Engine::Editor::AnimationClipEditing
{
using Channel = Engine::Model::AnimationChannel;
using Clip = Engine::Model::AnimationClip;

inline bool IsKeyed(const Clip& clip, unsigned node, float time,
    float tolerance = 1e-4f)
{
    for (const Channel& channel : clip.channels)
        if (channel.nodeIndex == node && channel.path != Channel::Path::Weights)
            for (float key : channel.times)
                if (std::abs(key - time) <= tolerance) return true;
    return false;
}

inline std::vector<float> KeyTimes(const Clip& clip, unsigned node)
{
    std::vector<float> times;
    for (const Channel& channel : clip.channels)
        if (channel.nodeIndex == node && channel.path != Channel::Path::Weights)
            times.insert(times.end(), channel.times.begin(), channel.times.end());
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
        [](float a, float b) { return std::abs(a - b) <= 1e-4f; }),
        times.end());
    return times;
}

inline std::optional<float> NearestKeyTime(const Clip& clip, unsigned node,
    float time, float tolerance)
{
    std::optional<float> nearest;
    float distance = tolerance;
    for (float key : KeyTimes(clip, node))
        if (std::abs(key - time) <= distance)
        {
            nearest = key;
            distance = std::abs(key - time);
        }
    return nearest;
}

inline void UpsertChannel(Clip& clip, unsigned node, Channel::Path path,
    float time, const float* values, unsigned width)
{
    auto found = std::find_if(clip.channels.begin(), clip.channels.end(),
        [&](const Channel& candidate)
        { return candidate.nodeIndex == node && candidate.path == path; });
    if (found == clip.channels.end())
    {
        clip.channels.emplace_back();
        found = std::prev(clip.channels.end());
        found->nodeIndex = node;
        found->path = path;
        found->valueWidth = width;
    }
    Channel& channel = *found;
    // Cubic spline stores in/value/out triples. Preserve its keyed values
    // when converting an edited channel to ordinary linear interpolation.
    if (channel.interpolation == Channel::Interpolation::CubicSpline)
    {
        std::vector<float> keyed;
        keyed.reserve(channel.times.size() * width);
        for (size_t key = 0; key < channel.times.size(); ++key)
            for (unsigned component = 0; component < width; ++component)
            {
                const size_t source = (key * 3 + 1) * width + component;
                keyed.push_back(source < channel.values.size()
                    ? channel.values[source] : 0.f);
            }
        channel.values = std::move(keyed);
        channel.interpolation = Channel::Interpolation::Linear;
    }
    channel.valueWidth = width;
    channel.values.resize(channel.times.size() * width, 0.f);
    auto at = std::lower_bound(channel.times.begin(), channel.times.end(), time);
    if (at != channel.times.begin() &&
        std::abs(*(at - 1) - time) <= 1e-4f)
        --at;
    const size_t index = static_cast<size_t>(at - channel.times.begin());
    if (at == channel.times.end() || std::abs(*at - time) > 1e-4f)
    {
        channel.times.insert(at, time);
        channel.values.insert(channel.values.begin() + index * width,
            values, values + width);
    }
    else std::copy(values, values + width,
        channel.values.begin() + index * width);
}

inline bool UpsertBonePose(Clip& clip, unsigned node, float time,
    const glm::vec3& translation, const glm::quat& rotation,
    const glm::vec3& scale)
{
    if (!std::isfinite(time) || time < 0.f ||
        !std::isfinite(glm::length(rotation)) ||
        glm::length(rotation) < 1e-6f ||
        !std::isfinite(translation.x) || !std::isfinite(translation.y) ||
        !std::isfinite(translation.z) || !std::isfinite(scale.x) ||
        !std::isfinite(scale.y) || !std::isfinite(scale.z)) return false;
    const glm::quat q = glm::normalize(rotation);
    const float position[]{ translation.x, translation.y, translation.z };
    const float quaternion[]{ q.x, q.y, q.z, q.w };
    const float size[]{ scale.x, scale.y, scale.z };
    UpsertChannel(clip, node, Channel::Path::Translation, time, position, 3);
    UpsertChannel(clip, node, Channel::Path::Rotation, time, quaternion, 4);
    UpsertChannel(clip, node, Channel::Path::Scale, time, size, 3);
    clip.duration = std::max(clip.duration, time);
    return true;
}

inline bool DeleteBoneKey(Clip& clip, unsigned node, float time)
{
    bool removed = false;
    for (auto it = clip.channels.begin(); it != clip.channels.end();)
    {
        if (it->nodeIndex == node && it->path != Channel::Path::Weights)
        {
            const unsigned stride = it->valueWidth *
                (it->interpolation == Channel::Interpolation::CubicSpline ? 3u : 1u);
            for (size_t key = it->times.size(); key-- > 0;)
                if (std::abs(it->times[key] - time) <= 1e-4f)
                {
                    it->times.erase(it->times.begin() + key);
                    const size_t start = key * stride;
                    if (start + stride <= it->values.size())
                        it->values.erase(it->values.begin() + start,
                            it->values.begin() + start + stride);
                    removed = true;
                }
            if (it->times.empty())
            { it = clip.channels.erase(it); continue; }
        }
        ++it;
    }
    return removed;
}
}
