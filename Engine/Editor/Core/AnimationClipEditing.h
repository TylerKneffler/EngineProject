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

struct KeyRef
{
    unsigned node = 0;
    float time = 0.f;
};

// One copied key keeps its channel identity and interpolation data. Its single
// time is relative to the first selected key on the editor timeline.
inline std::vector<Channel> CopyKeyBlocks(const Clip& clip, unsigned node,
    float sourceTime, float relativeTime)
{
    std::vector<Channel> result;
    for (const Channel& channel : clip.channels)
    {
        if (channel.nodeIndex != node || channel.path == Channel::Path::Weights)
            continue;
        const unsigned stride = channel.valueWidth *
            (channel.interpolation == Channel::Interpolation::CubicSpline ? 3u : 1u);
        if (!stride || channel.values.size() != channel.times.size() * stride)
            continue;
        for (size_t index = 0; index < channel.times.size(); ++index)
        {
            if (std::abs(channel.times[index] - sourceTime) > 1e-4f)
                continue;
            Channel copied = channel;
            copied.times = { relativeTime };
            copied.values.assign(channel.values.begin() + index * stride,
                channel.values.begin() + (index + 1) * stride);
            result.push_back(std::move(copied));
            break;
        }
    }
    return result;
}

inline bool PasteKeyBlocks(Clip& clip, const std::vector<Channel>& copied,
    float localStart)
{
    if (copied.empty() || !std::isfinite(localStart) || localStart < 0.f)
        return false;
    Clip result = clip;
    bool changed = false;
    for (const Channel& key : copied)
    {
        if (key.times.size() != 1 || !std::isfinite(key.times.front()) ||
            key.times.front() < 0.f) return false;
        const float time = localStart + key.times.front();
        if (!std::isfinite(time) || time < 0.f) return false;
        const unsigned sourceStride = key.valueWidth *
            (key.interpolation == Channel::Interpolation::CubicSpline ? 3u : 1u);
        if (!sourceStride || key.values.size() != sourceStride) return false;
        auto found = std::find_if(result.channels.begin(), result.channels.end(),
            [&](const Channel& channel)
            { return channel.nodeIndex == key.nodeIndex &&
                channel.path == key.path &&
                channel.targetMesh == key.targetMesh; });
        if (found == result.channels.end())
        {
            Channel inserted = key;
            inserted.times.front() = time;
            result.channels.push_back(std::move(inserted));
        }
        else
        {
            Channel& channel = *found;
            if (channel.valueWidth != key.valueWidth) return false;
            const unsigned destinationStride = channel.valueWidth *
                (channel.interpolation == Channel::Interpolation::CubicSpline ? 3u : 1u);
            if (channel.values.size() != channel.times.size() *
                    destinationStride) return false;
            std::vector<float> values;
            if (destinationStride == sourceStride &&
                channel.interpolation == key.interpolation)
                values = key.values;
            else if (channel.interpolation == Channel::Interpolation::CubicSpline)
            {
                values.assign(destinationStride, 0.f);
                const size_t sourceCenter =
                    key.interpolation == Channel::Interpolation::CubicSpline
                        ? key.valueWidth : 0;
                std::copy_n(key.values.begin() + sourceCenter,
                    key.valueWidth, values.begin() + key.valueWidth);
            }
            else
            {
                const size_t sourceCenter =
                    key.interpolation == Channel::Interpolation::CubicSpline
                        ? key.valueWidth : 0;
                values.assign(key.values.begin() + sourceCenter,
                    key.values.begin() + sourceCenter + key.valueWidth);
            }
            if (channel.targetPath.empty()) channel.targetPath = key.targetPath;
            auto at = std::lower_bound(channel.times.begin(),
                channel.times.end(), time);
            if (at != channel.times.begin() &&
                std::abs(*(at - 1) - time) <= 1e-4f)
                --at;
            const size_t index = static_cast<size_t>(at - channel.times.begin());
            if (at != channel.times.end() && std::abs(*at - time) <= 1e-4f)
                std::copy(values.begin(), values.end(),
                    channel.values.begin() + index * destinationStride);
            else
            {
                channel.times.insert(at, time);
                channel.values.insert(channel.values.begin() +
                    index * destinationStride, values.begin(), values.end());
            }
        }
        result.duration = std::max(result.duration, time);
        changed = true;
    }
    if (changed) clip = std::move(result);
    return changed;
}

// Move complete value blocks, including the tangents of cubic spline keys.
// Reject collisions instead of silently replacing another key.
inline bool MoveKeys(Clip& clip, const std::vector<KeyRef>& keys, float delta)
{
    if (keys.empty() || !std::isfinite(delta) || std::abs(delta) < 1e-6f)
        return false;
    for (const KeyRef& key : keys)
    {
        const float target = key.time + delta;
        if (!std::isfinite(target) || target < 0.f) return false;
        for (float existing : KeyTimes(clip, key.node))
            if (std::abs(existing - target) <= 1e-4f &&
                std::none_of(keys.begin(), keys.end(), [&](const KeyRef& moving)
                { return moving.node == key.node &&
                    std::abs(moving.time - existing) <= 1e-4f; }))
                return false;
    }
    Clip moved = clip;
    bool changed = false;
    for (Channel& channel : moved.channels)
    {
        if (channel.path == Channel::Path::Weights) continue;
        const size_t stride = channel.valueWidth *
            (channel.interpolation == Channel::Interpolation::CubicSpline ? 3u : 1u);
        if (!stride || channel.values.size() != channel.times.size() * stride)
            return false;
        std::vector<bool> selected(channel.times.size(), false);
        for (size_t i = 0; i < channel.times.size(); ++i)
            for (const KeyRef& key : keys)
                if (key.node == channel.nodeIndex &&
                    std::abs(key.time - channel.times[i]) <= 1e-4f)
                { selected[i] = true; changed = true; break; }
        for (size_t i = 0; i < channel.times.size(); ++i)
        {
            if (!selected[i]) continue;
            const float target = channel.times[i] + delta;
            if (!std::isfinite(target) || target < 0.f) return false;
            for (size_t j = 0; j < channel.times.size(); ++j)
                if (!selected[j] && std::abs(channel.times[j] - target) <= 1e-4f)
                    return false;
            channel.times[i] = target;
        }
        std::vector<size_t> order(channel.times.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
            { return channel.times[a] < channel.times[b]; });
        std::vector<float> times, values;
        times.reserve(order.size());
        values.reserve(channel.values.size());
        for (size_t i : order)
        {
            times.push_back(channel.times[i]);
            values.insert(values.end(), channel.values.begin() + i * stride,
                channel.values.begin() + (i + 1) * stride);
        }
        channel.times = std::move(times);
        channel.values = std::move(values);
    }
    if (!changed) return false;
    for (const KeyRef& key : keys)
        moved.duration = std::max(moved.duration, key.time + delta);
    clip = std::move(moved);
    return true;
}
}
