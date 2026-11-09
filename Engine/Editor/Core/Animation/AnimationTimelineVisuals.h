#pragma once
#include "Core/Model/AnimationClip.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "imgui.h"

namespace Engine::Editor::AnimationTimelineVisuals
{
inline constexpr size_t kAnimationDropdownLabelLength = 40;

inline std::string AnimationDropdownLabel(const std::string& label)
{
    size_t end = 0;
    size_t ellipsisEnd = 0;
    size_t characters = 0;
    while (end < label.size() && characters < kAnimationDropdownLabelLength)
    {
        if (characters == kAnimationDropdownLabelLength - 3)
            ellipsisEnd = end;
        const unsigned char byte = static_cast<unsigned char>(label[end]);
        const size_t length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 :
            byte < 0xF0 ? 3 : 4;
        end = std::min(label.size(), end + length);
        ++characters;
    }
    return end == label.size() ? label :
        label.substr(0, ellipsisEnd) + "...";
}

inline bool AnimationDropdownItem(const std::string& label, bool selected = false)
{
    const std::string display = AnimationDropdownLabel(label);
    const bool clicked = ImGui::Selectable(display.c_str(), selected);
    if (display != label && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", label.c_str());
    return clicked;
}

template <typename Document>
inline float AnimationLocalTime(const Document& document, size_t rigIndex,
    int clipIndex)
{
    const auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rigIndex](const auto& item)
        { return item.index == rigIndex; });
    if (track == document.tracks.end() || !track->sourceManager ||
        clipIndex < 0 || clipIndex >= static_cast<int>(
            track->sourceManager->clips.size()))
        return document.playheadSeconds;
    float start = 0.f;
    bool repeat = false;
    if (track->clipIndex == clipIndex)
    {
        start = track->startSeconds;
        repeat = track->repeat;
    }
    else
    {
        const auto layer = std::find_if(track->stackedClips.begin(),
            track->stackedClips.end(), [clipIndex](const auto& item)
            { return item.clipIndex == clipIndex; });
        if (layer != track->stackedClips.end())
        {
            start = layer->startSeconds;
            repeat = layer->repeat;
        }
    }
    const float length = std::max(.01f,
        track->sourceManager->clips[clipIndex].duration);
    const float elapsed = std::max(0.f, document.playheadSeconds - start);
    return repeat ? std::fmod(elapsed, length) : elapsed;
}

inline ImVec4 RigTimelineColor(size_t rigIndex)
{
    static const ImVec4 colors[] = {
        { .36f, .78f, 1.f, 1.f },
        { 1.f, .70f, .30f, 1.f },
        { .76f, .58f, 1.f, 1.f },
        { .48f, .84f, .58f, 1.f },
        { 1.f, .53f, .73f, 1.f },
        { .94f, .85f, .40f, 1.f },
        { .36f, .85f, .81f, 1.f },
        { 1.f, .53f, .43f, 1.f },
    };
    constexpr size_t colorCount = sizeof(colors) / sizeof(colors[0]);
    if (rigIndex < colorCount) return colors[rigIndex];
    const float hue = std::fmod(.17f +
        static_cast<float>(rigIndex - colorCount) * .6180339f, 1.f);
    ImVec4 generated{};
    ImGui::ColorConvertHSVtoRGB(hue, .62f, .92f,
        generated.x, generated.y, generated.z);
    generated.w = 1.f;
    return generated;
}

inline ImVec4 RigTimelineTint(ImVec4 color, float alpha)
{
    color.x *= .42f;
    color.y *= .42f;
    color.z *= .42f;
    color.w = alpha;
    return color;
}
inline float LastKeyTime(const Engine::Model::AnimationClip& clip)
{
    float result = 0.f;
    for (const auto& channel : clip.channels)
        if (!channel.times.empty())
            result = std::max(result, channel.times.back());
    return result;
}
}
