#pragma once
#include "Core/Compoonents/Animation/AnimationManager.h"
#include <algorithm>

namespace Engine::Editor::AnimationTimelineEditing
{
struct Playback
{
    struct ClipLayer
    {
        int clipIndex = -1;
        float weight = 1.f;
        float startSeconds = 0.f;
        bool repeat = false;
        int repeatCount = 0;
        Engine::Model::AnimationLayer settings;
    };
    int clipIndex = -1;
    float weight = 1.f;
    float startSeconds = 0.f;
    bool repeat = false;
    int repeatCount = 0;
    Engine::Model::AnimationLayer baseSettings;
    std::vector<ClipLayer> stackedClips;
};

inline void Read(Playback& playback,
    const Engine::Components::AnimationManager& manager,
    const std::vector<Engine::Model::AnimationClip>* originalClips = nullptr)
{
    playback = {};
    const auto& clips = originalClips ? *originalClips : manager.clips;
    const auto indexOf = [&](const std::string& name)
    {
        for (int index = 0; index < static_cast<int>(clips.size()); ++index)
            if (clips[index].clipName == name) return index;
        return -1;
    };
    if (manager.baseAnimationEnabled || (manager.timelinePlayback && !manager.clip.empty()))
    {
        playback.clipIndex = indexOf(manager.clip);
        if (playback.clipIndex < 0 && !manager.timelinePlayback && !clips.empty())
            playback.clipIndex = 0;
    }
    playback.repeat = manager.looping;
    for (const auto& layer : manager.layers)
    {
        const int index = indexOf(layer.clip);
        if (manager.timelinePlayback && index >= 0 && index == playback.clipIndex)
        {
            playback.baseSettings = layer;
            playback.weight = layer.weight;
            playback.startSeconds = layer.startSeconds;
            playback.repeat = layer.looping;
            playback.repeatCount = layer.repeatCount;
            continue;
        }
        if (index < 0 || index == playback.clipIndex ||
            std::any_of(playback.stackedClips.begin(), playback.stackedClips.end(),
                [&](const auto& existing) { return existing.clipIndex == index; })) continue;
        Playback::ClipLayer item;
        item.clipIndex = index;
        item.weight = layer.weight;
        item.startSeconds = layer.startSeconds;
        item.repeatCount = layer.repeatCount;
        item.repeat = layer.looping;
        item.settings = layer;
        playback.stackedClips.push_back(std::move(item));
    }
}

inline bool Contains(const Playback& playback, int index)
{
    return playback.clipIndex == index || std::any_of(
        playback.stackedClips.begin(), playback.stackedClips.end(),
        [&](const auto& layer) { return layer.clipIndex == index; });
}

inline void Remove(Playback& playback, int index)
{
    if (playback.clipIndex == index)
    {
        playback.clipIndex = -1;
        playback.startSeconds = 0.f;
        playback.weight = 1.f;
        playback.repeat = false;
        playback.repeatCount = 0;
        playback.baseSettings = {};
    }
    playback.stackedClips.erase(std::remove_if(playback.stackedClips.begin(),
        playback.stackedClips.end(), [&](const auto& layer)
        { return layer.clipIndex == index; }), playback.stackedClips.end());
}

inline void EraseClip(Playback& playback, int index)
{
    Remove(playback, index);
    if (playback.clipIndex > index) --playback.clipIndex;
    for (auto& layer : playback.stackedClips)
        if (layer.clipIndex > index) --layer.clipIndex;
}

inline void Write(const Playback& playback,
    Engine::Components::AnimationManager& manager)
{
    const bool primary = playback.clipIndex >= 0 &&
        playback.clipIndex < static_cast<int>(manager.clips.size());
    manager.baseAnimationEnabled = false;
    manager.timelinePlayback = true;
    manager.timelineTime = 0.f;
    manager.clip = primary ? manager.clips[playback.clipIndex].clipName : std::string();
    manager.layers.clear();
    if (primary)
    {
        auto layer = playback.baseSettings;
        layer.clip = manager.clip;
        layer.weight = playback.weight;
        layer.startSeconds = playback.startSeconds;
        layer.looping = playback.repeat;
        layer.repeatCount = playback.repeatCount;
        manager.layers.push_back(std::move(layer));
    }
    for (const auto& item : playback.stackedClips)
        if (item.clipIndex >= 0 && item.clipIndex < static_cast<int>(manager.clips.size()))
        {
            auto layer = item.settings;
            layer.clip = manager.clips[item.clipIndex].clipName;
            layer.weight = item.weight;
            layer.looping = item.repeat;
            layer.startSeconds = item.startSeconds;
            layer.repeatCount = item.repeatCount;
            manager.layers.push_back(std::move(layer));
        }
}
}
