#include "pch.h"
#include "Engine/Editor/State/EditorState.h"
#include "Core/Animation/AnimationTimelineVisuals.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/Animation/AnimationClipEditing.h"
#include "Core/Animation/AnimationClipMapping.h"
#include "Core/Animation/AnimationRigSelection.h"
#include "Core/Animation/AnimationEditorSource.h"
#include "Core/Importers/MmdDanceAssetBinder.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/Views/AnimationView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Engine/Editor/UI/EditorIcons.h"
#include "Core/Animation/SkeletonBindPose.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Model/AnimationControlSpace.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/ComponentReference.h"
#include "Core/Model/RigAsset.h"
#include "Core/Model/PoseAsset.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/AnimationRigMapping.h"
#include "Core/Object.h"
#include "Core/Prefab/PrefabAsset.h"
#include "imgui.h"
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine::Editor
{
using namespace AnimationTimelineVisuals;


void EditorState::DrawAnimationGraph(AnimationDocument& document,
    float duration)
{
    if (document.clipDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        document.clipDragging = false;
        const auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
            [&](const auto& item) { return item.index == document.clipDragRig; });
        if (track != document.tracks.end() && document.clipDragBefore)
        {
            FocusAnimationRig(document, track->index);
            RecordAnimationEdit(document, track->sourceManager->clips, document.clipDragBefore);
        }
        document.clipDragBefore.reset();
    }

    struct Row
    {
        enum class Kind { Header, Settings, Bone, Unresolved } kind = Kind::Header;
        AnimationDocument::RigTrack* track = nullptr;
        Engine::Core::Object* joint = nullptr;
        Engine::Model::AnimationClip* clip = nullptr;
        const std::unordered_map<unsigned, std::vector<float>>* keyTimes = nullptr;
        bool mappingReady = true;
        size_t boneIndex = 0;
        unsigned nodeIndex = 0;
        bool* expanded = nullptr;
        int clipIndex = -1;
        float height = 25.f;
        float offset = 0.f;
        float startSeconds = 0.f;
        bool repeat = false;
        int repeatCount = 0;
        const std::vector<AnimationDocument::CachedClip::SummaryKey>* summaryKeys = nullptr;
        const AnimationDocument::CachedClip::UnresolvedRow* unresolved = nullptr;
    };
    std::vector<Row> rows;
    for (auto& track : document.tracks)
    {
        if (!track.selected) continue;
        auto& clips = track.sourceManager->clips;
        const auto appendClip = [&](int clipIndex, bool& expanded,
            float startSeconds, bool repeat, int repeatCount)
        {
            Engine::Model::AnimationClip* clip = clipIndex >= 0 &&
                clipIndex < static_cast<int>(clips.size())
                    ? &clips[clipIndex] : nullptr;
            const std::unordered_map<unsigned, std::vector<float>>* keyTimes =
                nullptr;
            bool mappingReady = true;
            if (clip)
            {
                auto& cached = CachedAnimationClip(document, track, clipIndex);
                mappingReady = cached.mappingReady;
                keyTimes = &cached.keyTimes;
            }
            rows.push_back({ Row::Kind::Header, &track, nullptr, clip,
                keyTimes, mappingReady, 0, 0, &expanded, clipIndex, 44.f });
            rows.back().startSeconds = startSeconds;
            rows.back().repeat = repeat;
            rows.back().repeatCount = repeatCount;
            if (clip)
                rows.back().summaryKeys = &CachedAnimationClip(
                    document, track, clipIndex).summaryKeys;
            if (!expanded) return;
            if (clip)
            {
                const float settingsHeight = track.clipSettingsHeights.try_emplace(clipIndex, 440.f).first->second;
                rows.push_back({Row::Kind::Settings, &track, nullptr, clip,
                    nullptr, mappingReady, 0, 0, nullptr, clipIndex, settingsHeight});
            }
            if (clip)
                for (const auto& unresolved : CachedAnimationClip(
                    document, track, clipIndex).unresolvedRows)
                {
                    rows.push_back({ Row::Kind::Unresolved, &track,
                        nullptr, clip, keyTimes, false, 0, 0,
                        nullptr, clipIndex });
                    rows.back().startSeconds = startSeconds;
                    rows.back().unresolved = &unresolved;
                }
            const auto& joints = track.poseSkeleton->ResolveJoints();
            for (size_t boneIndex = 0; boneIndex < joints.size(); ++boneIndex)
            {
                auto* joint = joints[boneIndex];
                if (!joint) continue;
                auto* bone = joint->GetComponent<
                    Engine::Components::AnimationBone>();
                const unsigned nodeIndex = bone ? bone->nodeIndex :
                    boneIndex < track.poseSkeleton->jointNodes.size()
                        ? track.poseSkeleton->jointNodes[boneIndex] : 0;
                rows.push_back({ Row::Kind::Bone, &track, joint, clip,
                    keyTimes, mappingReady, boneIndex, nodeIndex,
                    nullptr, clipIndex });
                rows.back().startSeconds = startSeconds;
            }
        };
        if (track.clipIndex >= 0 &&
            track.clipIndex < static_cast<int>(clips.size()))
        {
            bool& expanded = track.expandedClips.try_emplace(
                track.clipIndex, false).first->second;
            appendClip(track.clipIndex, expanded, track.startSeconds,
                track.repeat, track.repeatCount);
        }
        for (const auto& layer : track.stackedClips)
            if (layer.clipIndex >= 0 &&
                layer.clipIndex < static_cast<int>(clips.size()) &&
                layer.clipIndex != track.clipIndex)
            {
                bool& expanded = track.expandedClips.try_emplace(
                    layer.clipIndex, false).first->second;
                appendClip(layer.clipIndex, expanded, layer.startSeconds,
                    layer.repeat, layer.repeatCount);
            }
    }
    float rowTotalHeight = 0.f;
    for (auto& row : rows)
    {
        row.offset = rowTotalHeight;
        rowTotalHeight += row.height;
    }
    constexpr float labelWidth = 292.f;
    constexpr float rulerHeight = 30.f;
    const float pixelsPerSecond = document.timelinePixelsPerSecond;
    const float seconds = std::max({ 5.f, duration + 1.f,
        document.playheadSeconds + 1.f });
    const float canvasWidth = std::max(ImGui::GetContentRegionAvail().x,
        labelWidth + seconds * pixelsPerSecond + 24.f);
    const float canvasHeight = rulerHeight + rowTotalHeight + 4.f;
    const float graphHeight = std::max(1.f,
        ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y);
    if (!ImGui::BeginChild("##AnimationGraph", { 0.f, graphHeight }, true,
            ImGuiWindowFlags_HorizontalScrollbar))
    { ImGui::EndChild(); return; }
    ImGui::Dummy({ canvasWidth, std::max(canvasHeight, graphHeight - 8.f) });
    const ImVec2 origin = ImGui::GetItemRectMin();
    const float timeX = origin.x + labelWidth;
    const float pinnedX = origin.x + ImGui::GetScrollX();
    const float labelRight = pinnedX + labelWidth;
    const float bottomY = origin.y + canvasHeight;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,
        { origin.x + canvasWidth, origin.y + rulerHeight },
        ImGui::GetColorU32(ImGuiCol_FrameBg));
    draw->AddLine({ timeX, origin.y }, { timeX, bottomY },
        ImGui::GetColorU32(ImGuiCol_Border));

    constexpr float steps[] = { .02f, .05f, .1f, .2f, .5f, 1.f, 2.f, 5.f,
        10.f, 20.f, 50.f, 100.f };
    float step = steps[std::size(steps) - 1];
    for (float candidate : steps)
        if (candidate * pixelsPerSecond >= 72.f)
        { step = candidate; break; }
    const float visibleStart = std::max(0.f,
        (ImGui::GetScrollX() - labelWidth) / pixelsPerSecond);
    const float visibleEnd = std::min(seconds,
        (ImGui::GetScrollX() + ImGui::GetWindowWidth() - labelWidth) /
            pixelsPerSecond + step);
    for (float time = std::ceil(visibleStart / step) * step;
        time <= visibleEnd + .0001f; time += step)
    {
        const float x = timeX + time * pixelsPerSecond;
        draw->AddLine({ x, origin.y + rulerHeight - 9.f },
            { x, bottomY }, IM_COL32(150, 160, 175, 52));
        char label[24];
        std::snprintf(label, sizeof(label),
            step >= 1.f ? "%.0f s" : "%.2f s", time);
        draw->AddText({ x + 4.f, origin.y + 5.f },
            ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    draw->AddRectFilled({ pinnedX, origin.y },
        { labelRight, origin.y + rulerHeight },
        ImGui::GetColorU32(ImGuiCol_FrameBg));
    draw->AddText({ pinnedX + 9.f, origin.y + 5.f },
        ImGui::GetColorU32(ImGuiCol_TextDisabled), "Animation / bone");

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsItemHovered();
    const bool overSettings = std::any_of(rows.begin(), rows.end(), [&](const Row& row)
    {
        const float top = origin.y + rulerHeight + row.offset;
        return row.kind == Row::Kind::Settings && mouse.y >= top && mouse.y < top + row.height;
    });
    const bool leftClicked = hovered && !overSettings &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool rightClicked = hovered && !overSettings &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const auto timeAt = [&](float x)
    {
        return std::clamp((x - timeX) / pixelsPerSecond, 0.f, seconds);
    };
    const bool onRuler = mouse.y >= origin.y &&
        mouse.y < origin.y + rulerHeight;
    if (hovered && mouse.x >= labelRight && onRuler)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (leftClicked && mouse.x >= labelRight && onRuler)
    {
        document.playheadDragging = true;
        document.playing = false;
    }
    if (document.playheadDragging)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            document.playheadDragging = false;
        else
        {
            const float time = timeAt(mouse.x);
            if (time != document.playheadSeconds)
            {
                document.playheadSeconds = time;
                SampleAnimationDocument(document);
            }
        }
    }

    const float visibleTop = ImGui::GetWindowPos().y;
    const float visibleBottom = visibleTop + ImGui::GetWindowHeight();
    AnimationDocument::RigTrack* removeLayerFrom = nullptr;
    int removeTimelineClip = -1;
    AnimationDocument::RigTrack* removeLibraryFrom = nullptr;
    int removeLibraryClip = -1;
    AnimationDocument::RigTrack* editedTrack = nullptr;
    std::vector<Engine::Model::AnimationClip> editBefore;
    bool refreshClipCache = false;
    const auto drawClipProperties = [&](Row& row)
    {
        auto& clips = row.track->sourceManager->clips;
        const int clipIndex = row.clipIndex;
        auto& clip = clips[clipIndex];
        ImGui::TextDisabled("Animation asset");
        int rigIkOverride = static_cast<int>(clip.rigIkOverride);
        const char* ikOptions[] = {"Use model settings", "Disable rig IK (FK animation)", "Enable rig IK"};
        ImGui::SetNextItemWidth(240.f);
        if (ImGui::Combo("Rig IK override", &rigIkOverride, ikOptions, 3))
        {
            editBefore = clips;
            editedTrack = row.track;
            clip.rigIkOverride = static_cast<Engine::Model::AnimationClip::RigIkOverride>(rigIkOverride);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saved in the .anim. Overrides imported rig IK while this animation plays. "
                "The last active layer with an explicit override takes precedence.");
        ImGui::Separator();
        const bool primary = row.track->clipIndex == clipIndex;
        auto stacked = std::find_if(row.track->stackedClips.begin(),
            row.track->stackedClips.end(), [&](const auto& layer)
            { return layer.clipIndex == clipIndex; });
        const bool isStacked = stacked != row.track->stackedClips.end();
        ImGui::TextDisabled("Preview: %s", primary ? "Primary animation"
            : isStacked ? "Stacked animation" : "Available");
        float inactiveWeight = 0.f;
        float& weight = primary ? row.track->weight
            : isStacked ? stacked->weight : inactiveWeight;
        ImGui::BeginDisabled(!primary && !isStacked);
        auto& settings = primary ? row.track->baseSettings : stacked->settings;
        if (ImGui::Checkbox("Enabled", &settings.enabled))
            AnimationPlaybackChanged(document, false);

        if (ImGui::SliderFloat("Weight", &weight, 0.f, 1.f, "%.2f"))
            AnimationPlaybackChanged(document, false);
        float& start = primary ? row.track->startSeconds
            : stacked->startSeconds;
        bool& repeat = primary ? row.track->repeat : stacked->repeat;
        int& count = primary ? row.track->repeatCount : stacked->repeatCount;
        if (ImGui::DragFloat("Start (seconds)", &start, .01f,
                0.f, 3600.f, "%.2f s"))
        {
            start = std::max(0.f, start);
            AnimationPlaybackChanged(document, false);
        }
        if (ImGui::RadioButton("Isolated (play once)", !repeat))
        {
            repeat = false;
            AnimationPlaybackChanged(document, false);
        }
        if (ImGui::RadioButton("Repeat", repeat))
        {
            repeat = true;
            AnimationPlaybackChanged(document, false);
        }
        if (repeat)
        {
            bool infinite = count == 0;
            if (ImGui::Checkbox("Infinite", &infinite))
            {
                count = infinite ? 0 : 2;
                AnimationPlaybackChanged(document, false);
            }
            if (!infinite && ImGui::InputInt("Number of plays", &count))
            {
                count = std::max(1, count);
                AnimationPlaybackChanged(document, false);
            }
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextDisabled("Clip timing");
        float length = clip.duration;
        const bool lengthChanged = ImGui::DragFloat("Length (seconds)",
            &length, .01f, .01f, 3600.f, "%.2f s");
        if (ImGui::IsItemActivated())
        {
            document.durationDragBefore = clips;
            document.durationDragClip = clipIndex;
            document.durationDragChanged = false;
        }
        if (lengthChanged)
        {
            if (document.durationDragBefore.empty())
            {
                document.durationDragBefore = clips;
                document.durationDragClip = clipIndex;
            }
            const float next = std::max({ .01f, length, LastKeyTime(clip) });
            if (next != clip.duration)
            {
                clip.duration = next;
                document.durationDragChanged = true;
                refreshClipCache = true;
            }
        }
        if (ImGui::IsItemDeactivated() &&
            document.durationDragClip == clipIndex)
        {
            if (document.durationDragChanged)
            {
                editedTrack = row.track;
                editBefore = std::move(document.durationDragBefore);
            }
            document.durationDragBefore.clear();
            document.durationDragClip = -1;
            document.durationDragChanged = false;
        }
        bool loop = clip.looping;
        if (ImGui::Checkbox("Loop", &loop))
        {
            editBefore = clips;
            editedTrack = row.track;
            clip.looping = loop;
        }
        const float clipLength = std::max(clip.duration, .01f);
        const float elapsed = std::max(document.playheadSeconds - start, 0.f);
        const float localTime = repeat
            ? std::fmod(elapsed, clipLength)
            : std::min(elapsed, clipLength);
        ImGui::TextDisabled("Local time %.2f / %.2f s", localTime,
            clipLength);
        if (ImGui::Button("Remove from timeline"))
        {
            removeLayerFrom = row.track;
            removeTimelineClip = row.clipIndex;
        }
        if (ImGui::Button("Remove from model / pack"))
        {
            removeLibraryFrom = row.track;
            removeLibraryClip = row.clipIndex;
        }
    };
    const auto activateClip = [&](Row& row)
    {
        if (document.rigIndex != row.track->index)
            FocusAnimationRig(document, row.track->index);
        document.clipIndex = row.clipIndex;
    };
    using TimelineKey = AnimationDocument::TimelineKey;
    struct KeyVisual
    {
        Row* row = nullptr;
        float time = 0.f;
        float x = 0.f;
        float y = 0.f;
        std::vector<TimelineKey> keys;
    };
    std::vector<KeyVisual> keyVisuals;
    // Ordinary hit testing only needs the row under the mouse. A marquee
    // needs every visible row while it is active or starting this frame.
    const bool collectAllKeyVisuals = document.marqueeActive || rightClicked;
    const bool collectHitRowVisuals = hovered && mouse.x >= labelRight;
    const auto sameKey = [](const TimelineKey& a, const TimelineKey& b)
    {
        return a.rigIndex == b.rigIndex && a.clipIndex == b.clipIndex &&
            a.nodeIndex == b.nodeIndex && std::abs(a.time - b.time) <= 1e-4f;
    };
    // Header keys can contain hundreds of bones. Index selection once so
    // drawing a large selection does not scan every selected key per bone.
    std::map<std::tuple<size_t, int, unsigned>, std::vector<float>> selectedTimes;
    for (const auto& key : document.selectedKeys)
        selectedTimes[{key.rigIndex, key.clipIndex, key.nodeIndex}].push_back(key.time);
    for (auto& [identity, times] : selectedTimes)
        std::sort(times.begin(), times.end());
    const auto isDrawSelected = [&](const TimelineKey& key)
    {
        const auto found = selectedTimes.find(
            {key.rigIndex, key.clipIndex, key.nodeIndex});
        if (found == selectedTimes.end()) return false;
        const auto& times = found->second;
        const auto next = std::lower_bound(times.begin(), times.end(), key.time);
        return (next != times.end() && std::abs(*next - key.time) <= 1e-4f) ||
            (next != times.begin() && std::abs(*std::prev(next) - key.time) <= 1e-4f);
    };
    // Input handling below mutates selection during this frame.
    const auto isSelected = [&](const TimelineKey& key)
    {
        return std::any_of(document.selectedKeys.begin(),
            document.selectedKeys.end(), [&](const TimelineKey& selected)
            { return sameKey(key, selected); });
    };
    const auto drawKey = [&](Row& row, float center, float time,
        const std::vector<TimelineKey>& keys, ImU32 color)
    {
        const bool selected = std::any_of(keys.begin(), keys.end(), isDrawSelected);
        float shownTime = time;
        if (document.keyDragging && selected &&
            std::all_of(keys.begin(), keys.end(), isDrawSelected))
            shownTime += document.keyDragDelta;
        const float x = timeX +
            (row.startSeconds + shownTime) * pixelsPerSecond;
        if (x < labelRight - 8.f || x > draw->GetClipRectMax().x + 8.f)
            return;
        if (selected)
            draw->AddCircleFilled({ x, center }, 10.f,
                IM_COL32(255, 225, 125, 95));
        draw->AddQuadFilled({ x, center - 6.f },
            { x + 6.f, center }, { x, center + 6.f },
            { x - 6.f, center }, selected
                ? IM_COL32(255, 226, 135, 255) : color);
        if (selected)
            draw->AddCircle({ x, center }, 8.f,
                IM_COL32(255, 245, 195, 255), 0, 1.5f);
        if (collectAllKeyVisuals ||
            (collectHitRowVisuals &&
                std::abs(mouse.y - center) <= row.height * .5f))
            keyVisuals.push_back({ &row, time, x, center, keys });
    };
    using TimelineRow = AnimationDocument::TimelineRow;
    const auto rowIdentity = [](const Row& row)
    { return TimelineRow{row.track->index, row.clipIndex, row.nodeIndex}; };
    const auto sameRow = [](const TimelineRow& left,
        const TimelineRow& right)
    {
        return left.rigIndex == right.rigIndex &&
            left.clipIndex == right.clipIndex &&
            left.nodeIndex == right.nodeIndex;
    };
    const auto selectedRow = [&](const Row& row)
    {
        const auto identity = rowIdentity(row);
        return std::any_of(document.selectedRows.begin(),
            document.selectedRows.end(), [&](const auto& selected)
            { return sameRow(identity, selected); });
    };
    const auto selectRow = [&](const Row& row, bool toggle, bool range)
    {
        const auto identity = rowIdentity(row);
        if (!document.selectedRows.empty() &&
            document.selectedRows.front().rigIndex != identity.rigIndex)
        {
            document.selectedRows.clear();
            document.rowSelectionAnchor.reset();
        }
        if (range && document.rowSelectionAnchor &&
            document.rowSelectionAnchor->rigIndex == identity.rigIndex)
        {
            const auto locate = [&](const TimelineRow& target)
            {
                for (size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].kind == Row::Kind::Bone &&
                        sameRow(rowIdentity(rows[i]), target)) return i;
                return rows.size();
            };
            const size_t first = locate(*document.rowSelectionAnchor);
            const size_t last = locate(identity);
            if (first < rows.size() && last < rows.size())
            {
                if (!toggle) document.selectedRows.clear();
                for (size_t i = std::min(first, last);
                    i <= std::max(first, last); ++i)
                {
                    if (rows[i].kind != Row::Kind::Bone ||
                        rows[i].track->index != identity.rigIndex) continue;
                    const auto candidate = rowIdentity(rows[i]);
                    if (std::none_of(document.selectedRows.begin(),
                            document.selectedRows.end(), [&](const auto& selected)
                            { return sameRow(candidate, selected); }))
                        document.selectedRows.push_back(candidate);
                }
                return;
            }
        }
        if (toggle)
        {
            auto found = std::find_if(document.selectedRows.begin(),
                document.selectedRows.end(), [&](const auto& selected)
                { return sameRow(identity, selected); });
            if (found == document.selectedRows.end())
                document.selectedRows.push_back(identity);
            else document.selectedRows.erase(found);
        }
        else
        {
            document.selectedRows.clear();
            document.selectedRows.push_back(identity);
        }
        document.rowSelectionAnchor = identity;
    };
    std::vector<TimelineKey> keyGroup;
    std::vector<TimelineKey> singleKey;
    singleKey.reserve(1);
    struct PendingAssignment
    {
        size_t rig = 0;
        int clip = -1;
        size_t channel = 0;
        size_t bone = 0;
        bool ready = false;
    } pendingAssignment;
    const auto drawAssignmentPicker = [&](Row& row,
        size_t channelIndex, const std::string& boneName,
        ImU32 textColor, float top)
    {
        if (!row.clip || channelIndex >= row.clip->channels.size()) return;
        const auto& channel = row.clip->channels[channelIndex];
        ImGui::PushID(row.track);
        ImGui::PushID(row.clipIndex);
        ImGui::PushID(static_cast<int>(channelIndex));
        const float nameWidth = std::min(
            ImGui::CalcTextSize(boneName.c_str()).x + 8.f,
            labelWidth - 20.f);
        ImGui::SetCursorScreenPos({ pinnedX + 8.f, top + 2.f });
        if (ImGui::InvisibleButton("##BoneName", { nameWidth, 20.f }))
        {
            if (document.rigIndex != row.track->index)
                FocusAnimationRig(document, row.track->index);
            document.clipIndex = row.clipIndex;
            document.repairIssueChannel = static_cast<int>(channelIndex);
            ImGui::OpenPopup("##AssignBone");
        }
        const bool nameHovered = ImGui::IsItemHovered();
        draw->PushClipRect({ pinnedX + 12.f, top },
            { labelRight - 8.f, top + row.height }, true);
        draw->AddText({ pinnedX + 12.f, top + 4.f },
            textColor, boneName.c_str());
        if (nameHovered)
            draw->AddLine({ pinnedX + 12.f, top + 20.f },
                { pinnedX + 8.f + nameWidth, top + 20.f }, textColor);
        draw->PopClipRect();
        if (nameHovered)
            ImGui::SetTooltip("%s\nClick to choose a bone",
                channel.targetPath.c_str());
        if (ImGui::BeginPopup("##AssignBone"))
        {
            ImGui::TextUnformatted(channel.targetPath.c_str());
            const auto bones = AnimationClipMapping::RigNodes(
                *row.track->poseSkeleton);
            for (size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex)
                if (ImGui::Selectable(bones[boneIndex].path.c_str(),
                        bones[boneIndex].nodeIndex == channel.nodeIndex))
                    pendingAssignment = {row.track->index, row.clipIndex,
                        channelIndex, boneIndex, true};
            ImGui::Separator();
            if (ImGui::Selectable("Pick bone in 3D view"))
            {
                document.repairIssueChannel = static_cast<int>(channelIndex);
                document.repairRigIndex = row.track->index;
                document.repairClipIndex = row.clipIndex;
                document.repairAwaitingViewportBone = true;
                document.view->SetEditMode(SceneEditMode::Skeleton);
                document.view->SetPoseEditing(true);
                document.view->SetModelManipulation(true);
                document.view->SetModelSelectionMode(1);
                document.poseScene->SetEditorBonesVisible(true);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    };
    for (size_t index = 0; index < rows.size(); ++index)
    {
        Row& row = rows[index];
        const float top = origin.y + rulerHeight + row.offset;
        const float bottom = top + row.height;
        if (row.kind != Row::Kind::Header &&
            (bottom < visibleTop || top > visibleBottom)) continue;
        const float center = top + row.height * .5f;
        const ImVec4 rigColor = RigTimelineColor(row.track->index);
        const ImU32 color = ImGui::GetColorU32(rigColor);
        const bool mouseInRow = hovered && mouse.y >= top &&
            mouse.y < bottom;
        if (row.kind == Row::Kind::Settings)
        {
            draw->AddRectFilled({pinnedX, top}, {draw->GetClipRectMax().x, bottom},
                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::PushID(row.track);
            ImGui::PushID(row.clipIndex);
            ImGui::SetCursorScreenPos({pinnedX + 8.f, top + 4.f});
            const float width = std::max(100.f, draw->GetClipRectMax().x - pinnedX - 16.f);
            if (ImGui::BeginChild("##AnimationSettings", {width, std::max(1.f, row.height - 8.f)},
                    false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                ImGui::SetCursorPos({8.f, 8.f});
                ImGui::PushItemWidth(std::min(240.f, width * .5f));
                drawClipProperties(row);
                ImGui::PopItemWidth();
                row.track->clipSettingsHeights[row.clipIndex] = ImGui::GetCursorPosY() + 16.f;
            }
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
        if (row.kind == Row::Kind::Header)
        {
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor, .42f)));
            draw->AddRectFilled({ pinnedX, top },
                { labelRight, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor,
                    row.track->clipIndex == row.clipIndex ? .72f : .42f)));
            if (row.clip)
            {
                const float length = std::max(.01f, row.clip->duration);
                const int plays = row.repeat && row.repeatCount > 0
                    ? row.repeatCount : 1;
                const float barStart = timeX +
                    row.startSeconds * pixelsPerSecond;
                const float barEnd = timeX +
                    (row.startSeconds + length * plays) * pixelsPerSecond;
                draw->AddRectFilled({ std::max(barStart, labelRight), bottom - 20.f },
                    { barEnd, bottom - 2.f }, color, 2.f);
                if (row.repeat && row.repeatCount == 0)
                    draw->AddText({ barEnd + 4.f, bottom - 16.f }, color,
                        "infinite");
                if (mouseInRow && mouse.y >= bottom - 21.f &&
                    mouse.x >= barStart && mouse.x <= barEnd &&
                    mouse.x >= labelRight)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    ImGui::SetTooltip("Drag this bar to move the animation. Right-click for clip actions.");
                    if (leftClicked)
                    {
                        activateClip(row);
                        document.clipDragBefore = *row.track;
                        document.clipDragging = true;
                        document.clipDragRig = row.track->index;
                        document.clipDragIndex = row.clipIndex;
                        document.clipDragMouseX = mouse.x;
                        document.clipDragStart = row.startSeconds;
                        document.playing = false;
                    }
                }
                if (document.clipDragging &&
                    document.clipDragRig == row.track->index &&
                    document.clipDragIndex == row.clipIndex)
                {
                    float& start = row.track->clipIndex == row.clipIndex
                        ? row.track->startSeconds
                        : std::find_if(row.track->stackedClips.begin(),
                            row.track->stackedClips.end(),
                            [&](const auto& layer)
                            { return layer.clipIndex == row.clipIndex; })
                            ->startSeconds;
                    const float moved = std::max(0.f,
                        document.clipDragStart +
                        (mouse.x - document.clipDragMouseX) /
                        pixelsPerSecond);
                    if (start != moved)
                    {
                        start = moved;
                        row.startSeconds = moved;
                        AnimationPlaybackChanged(document, false);
                    }
                }
            }
            const std::string label = row.clip
                ? row.clip->clipName : "No animation";
            const std::string rigLabel = row.track->name;
            const ImVec2 rigSize = ImGui::CalcTextSize(rigLabel.c_str());
            const bool showRig = rigSize.x < labelWidth * .35f;
            const float cogX = labelRight - 27.f;
            const ImVec2 arrowCenter{ pinnedX + 12.f, center };
            if (*row.expanded)
                draw->AddTriangleFilled(
                    { arrowCenter.x - 5.f, arrowCenter.y - 3.f },
                    { arrowCenter.x + 5.f, arrowCenter.y - 3.f },
                    { arrowCenter.x, arrowCenter.y + 4.f }, color);
            else
                draw->AddTriangleFilled(
                    { arrowCenter.x - 3.f, arrowCenter.y - 5.f },
                    { arrowCenter.x - 3.f, arrowCenter.y + 5.f },
                    { arrowCenter.x + 4.f, arrowCenter.y }, color);
            draw->PushClipRect({ pinnedX + 24.f, top },
                { std::max(pinnedX + 25.f,
                    cogX - (showRig ? rigSize.x : 0.f) - 12.f),
                    bottom }, true);
            draw->AddText({ pinnedX + 25.f, top + 5.f }, color,
                label.c_str());
            draw->PopClipRect();
            if (showRig)
                draw->AddText({ cogX - rigSize.x - 5.f, top + 5.f },
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    rigLabel.c_str());
            if (mouseInRow && mouse.x < cogX)
                ImGui::SetTooltip("%s / %s", label.c_str(),
                    rigLabel.c_str());
            if (mouseInRow && leftClicked &&
                mouse.x < cogX && !document.playheadDragging)
            {
                activateClip(row);
                *row.expanded = !*row.expanded;
            }
            ImGui::PushID(row.track);
            ImGui::PushID(row.clipIndex);
            ImGui::SetCursorScreenPos({ cogX, top + 3.f });
            if (ImGui::Button(Icons::Settings, { 22.f, 22.f }))
            {
                activateClip(row);
                *row.expanded = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Expand animation settings");
            ImGui::PopID();
            ImGui::PopID();
            ImGui::PushID(row.track);
            ImGui::PushID(row.clipIndex);
            if (mouseInRow && rightClicked)
                ImGui::OpenPopup("##ClipActions");
            if (ImGui::BeginPopup("##ClipActions"))
            {
                if (ImGui::MenuItem("Remove from timeline"))
                { removeLayerFrom = row.track; removeTimelineClip = row.clipIndex; }
                if (ImGui::MenuItem("Remove from model / pack"))
                { removeLibraryFrom = row.track; removeLibraryClip = row.clipIndex; }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::PopID();
            if (row.summaryKeys && bottom >= visibleTop && top <= visibleBottom)
            {
                const float drag = document.keyDragging ? document.keyDragDelta : 0.f;
                const float firstTime = visibleStart - row.startSeconds -
                    8.f / pixelsPerSecond - std::max(0.f, drag);
                const float lastTime = visibleEnd - row.startSeconds +
                    8.f / pixelsPerSecond - std::min(0.f, drag);
                for (auto key = std::lower_bound(row.summaryKeys->begin(),
                         row.summaryKeys->end(), firstTime,
                         [](const auto& group, float time) { return group.time < time; });
                     key != row.summaryKeys->end() && key->time <= lastTime; ++key)
                {
                    keyGroup.clear();
                    keyGroup.reserve(key->keys.size());
                    for (const auto& [node, time] : key->keys)
                        keyGroup.push_back({row.track->index, row.clipIndex, node, time});
                    drawKey(row, center, key->time, keyGroup, color);
                }
            }
            continue;
        }
        if (row.kind == Row::Kind::Unresolved)
        {
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                IM_COL32(170, 85, 35, 28));
            draw->AddRectFilled({ pinnedX, top }, { labelRight, bottom },
                IM_COL32(65, 42, 30, 255));
            const auto& unresolved = *row.unresolved;
            const std::string label = "Unassigned: " + (unresolved.path.empty()
                ? "Node " + std::to_string(unresolved.nodeIndex)
                : unresolved.path.substr(
                    unresolved.path.find_last_of('/') == std::string::npos
                        ? 0 : unresolved.path.find_last_of('/') + 1));
            for (float time : unresolved.times)
            {
                const float x = timeX + (row.startSeconds + time) *
                    pixelsPerSecond;
                if (x >= labelRight && x <= draw->GetClipRectMax().x)
                    draw->AddCircleFilled({ x, center }, 3.5f,
                        IM_COL32(255, 160, 90, 230));
            }
            drawAssignmentPicker(row, unresolved.channelIndex, label,
                IM_COL32(255, 170, 110, 255), top);
            continue;
        }
        if (index % 2 == 0)
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                IM_COL32(130, 140, 160, 13));
        const bool activeBone = document.rigIndex == row.track->index &&
            document.selectedBone == static_cast<int>(row.boneIndex);
        const bool rowIsSelected = selectedRow(row);
        if (rowIsSelected || activeBone)
            draw->AddRectFilled({ origin.x, top },
                { origin.x + canvasWidth, bottom },
                ImGui::GetColorU32(RigTimelineTint(rigColor,
                    rowIsSelected ? .4f : .25f)));
        draw->AddLine({ origin.x, bottom },
            { origin.x + canvasWidth, bottom },
            IM_COL32(150, 160, 175, 28));

        const auto times = row.keyTimes
            ? row.keyTimes->find(row.nodeIndex)
            : std::unordered_map<unsigned, std::vector<float>>::const_iterator{};
        if (row.keyTimes && times != row.keyTimes->end())
        for (auto time = std::lower_bound(times->second.begin(),
                 times->second.end(), visibleStart - row.startSeconds -
                     8.f / pixelsPerSecond - std::max(0.f,
                         document.keyDragging ? document.keyDragDelta : 0.f));
             time != times->second.end() &&
                 *time <= visibleEnd - row.startSeconds + 8.f / pixelsPerSecond -
                     std::min(0.f, document.keyDragging ? document.keyDragDelta : 0.f); ++time)
        {
            singleKey.clear();
            singleKey.push_back({ row.track->index, row.clipIndex,
                row.nodeIndex, *time });
            drawKey(row, center, *time, singleKey, color);
        }
        draw->AddRectFilled({ pinnedX, top },
            { labelRight, bottom },
            rowIsSelected
                ? ImGui::GetColorU32(RigTimelineTint(rigColor, .5f))
                : ImGui::GetColorU32(ImGuiCol_WindowBg));
        size_t assignedChannel = 0;
        if (row.clip)
            for (; assignedChannel < row.clip->channels.size();
                ++assignedChannel)
                if (const auto& channel = row.clip->channels[assignedChannel];
                    channel.nodeIndex == row.nodeIndex &&
                    !channel.targetPath.empty()) break;
        const bool canReassign = row.clip &&
            assignedChannel < row.clip->channels.size();
        if (canReassign)
            drawAssignmentPicker(row, assignedChannel, row.joint->name,
                activeBone || rowIsSelected ? color :
                    ImGui::GetColorU32(ImGuiCol_Text), top);
        else
        {
            draw->PushClipRect({ pinnedX + 12.f, top },
                { labelRight, bottom }, true);
            draw->AddText({ pinnedX + 12.f, top + 4.f },
                activeBone || rowIsSelected ? color :
                    ImGui::GetColorU32(ImGuiCol_Text),
                row.joint->name.c_str());
            draw->PopClipRect();
        }
        if (mouseInRow && mouse.x < labelRight && leftClicked &&
            !document.playheadDragging)
        {
            activateClip(row);
            selectRow(row, ImGui::GetIO().KeyCtrl,
                ImGui::GetIO().KeyShift);
            document.selectedBone = static_cast<int>(row.boneIndex);
            document.poseScene->SetSelectedObject(row.joint);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(row.joint);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(row.joint);
        }
    }
    const auto addSelection = [&](const TimelineKey& key)
    {
        if (!isSelected(key)) document.selectedKeys.push_back(key);
    };
    const auto findRow = [&](float y) -> Row*
    {
        for (Row& row : rows)
        {
            const float top = origin.y + rulerHeight + row.offset;
            if (y >= top && y < top + row.height)
                return row.kind == Row::Kind::Settings ? nullptr : &row;
        }
        return nullptr;
    };
    KeyVisual* hit = nullptr;
    for (KeyVisual& visual : keyVisuals)
        if (mouse.x >= labelRight && std::abs(mouse.x - visual.x) <= 8.f &&
            std::abs(mouse.y - visual.y) <= visual.row->height * .5f &&
            (!hit || std::abs(mouse.x - visual.x) <
                std::abs(mouse.x - hit->x))) hit = &visual;
    const bool additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
    if (rightClicked && mouse.x >= labelRight && !onRuler &&
        (!findRow(mouse.y) || findRow(mouse.y)->kind != Row::Kind::Header))
    {
        document.marqueeActive = true;
        document.marqueeStartX = mouse.x;
        document.marqueeStartY = mouse.y;
        document.marqueeAdditive = additive;
    }
    if (document.marqueeActive)
    {
        const float minX = std::min(document.marqueeStartX, mouse.x);
        const float maxX = std::max(document.marqueeStartX, mouse.x);
        const float minY = std::min(document.marqueeStartY, mouse.y);
        const float maxY = std::max(document.marqueeStartY, mouse.y);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            draw->AddRectFilled({ minX, minY }, { maxX, maxY },
                IM_COL32(100, 160, 255, 48));
            draw->AddRect({ minX, minY }, { maxX, maxY },
                IM_COL32(130, 185, 255, 220));
        }
        else
        {
            if (!document.marqueeAdditive) document.selectedKeys.clear();
            const size_t rig = [&]()
            {
                for (const KeyVisual& visual : keyVisuals)
                    if (visual.x >= minX && visual.x <= maxX &&
                        visual.y >= minY && visual.y <= maxY)
                        return visual.row->track->index;
                return static_cast<size_t>(-1);
            }();
            if (rig != static_cast<size_t>(-1))
            {
                if (!document.selectedKeys.empty() &&
                    document.selectedKeys.front().rigIndex != rig)
                    document.selectedKeys.clear();
                for (const KeyVisual& visual : keyVisuals)
                    if (visual.row->track->index == rig &&
                        visual.x >= minX && visual.x <= maxX &&
                        visual.y >= minY && visual.y <= maxY)
                        for (const TimelineKey& key : visual.keys)
                            addSelection(key);
            }
            document.marqueeActive = false;
        }
    }
    if (leftClicked && mouse.x >= labelRight && !onRuler &&
        !document.clipDragging &&
        !document.marqueeActive && !document.playheadDragging)
    {
        Row* row = findRow(mouse.y);
        if (hit && hit->row->mappingReady &&
            !(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                hit->row->kind == Row::Kind::Header))
        {
            activateClip(*hit->row);
            if (hit->row->kind == Row::Kind::Bone)
            {
                if (!additive) selectRow(*hit->row, false, false);
                document.selectedBone = static_cast<int>(hit->row->boneIndex);
                document.poseScene->SetSelectedObject(hit->row->joint);
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(hit->row->joint);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(hit->row->joint);
            }
            const bool alreadySelected = std::any_of(hit->keys.begin(),
                hit->keys.end(), isSelected);
            if (!additive && !alreadySelected)
                document.selectedKeys.clear();
            if (!document.selectedKeys.empty() &&
                document.selectedKeys.front().rigIndex !=
                    hit->row->track->index)
                document.selectedKeys.clear();
            for (const TimelineKey& key : hit->keys)
            {
                if (additive && isSelected(key))
                    document.selectedKeys.erase(std::remove_if(
                        document.selectedKeys.begin(),
                        document.selectedKeys.end(),
                        [&](const TimelineKey& item)
                        { return sameKey(item, key); }),
                        document.selectedKeys.end());
                else addSelection(key);
            }
            document.keyPressX = mouse.x;
            document.keyPressActive = true;
            document.keyDragging = false;
            document.keyDragDelta = 0.f;
            document.playheadSeconds = hit->row->startSeconds + hit->time;
            document.playing = false;
            SampleAnimationDocument(document);
        }
        else if (row && row->clip && row->mappingReady &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            activateClip(*row);
            document.playheadSeconds = hit
                ? hit->row->startSeconds + hit->time : timeAt(mouse.x);
            document.playing = false;
            SampleAnimationDocument(document);
            if (row->kind == Row::Kind::Bone && selectedRow(*row) &&
                document.selectedRows.size() > 1)
                AddAnimationRowKeys(document);
            else if (auto* model = row->track->poseSkeleton->ResolveModel())
            {
                auto before = row->track->sourceManager->clips;
                auto& clip = row->track->sourceManager->clips[row->clipIndex];
                const auto& joints = row->track->poseSkeleton->ResolveJoints();
                bool keyed = false;
                for (size_t boneIndex = 0; boneIndex < joints.size(); ++boneIndex)
                {
                    if (row->kind == Row::Kind::Bone &&
                        boneIndex != row->boneIndex) continue;
                    auto* joint = joints[boneIndex];
                    if (!joint) continue;
                    auto* bone = joint->GetComponent<
                        Engine::Components::AnimationBone>();
                    if (!bone && boneIndex >=
                        row->track->poseSkeleton->jointNodes.size()) continue;
                    const unsigned node = bone ? bone->nodeIndex :
                        row->track->poseSkeleton->jointNodes[boneIndex];
                    const auto& transform = joint->transform;
                    if (!AnimationClipEditing::UpsertBonePose(clip, node,
                            AnimationLocalTime(document, row->track->index,
                                row->clipIndex), transform.position,
                            glm::quat(transform.rotation), transform.scale))
                        continue;
                    const std::string targetPath =
                        AnimationClipMapping::NodePath(*model, node);
                    for (auto& channel : clip.channels)
                        if (channel.nodeIndex == node &&
                            channel.targetPath.empty())
                            channel.targetPath = targetPath;
                    keyed = true;
                }
                if (keyed) RecordAnimationEdit(document, std::move(before));
            }
        }
        else if (row && row->kind == Row::Kind::Bone)
        {
            activateClip(*row);
            selectRow(*row, ImGui::GetIO().KeyCtrl,
                ImGui::GetIO().KeyShift);
            document.selectedBone = static_cast<int>(row->boneIndex);
            document.poseScene->SetSelectedObject(row->joint);
            if (m_primaryHierarchy)
                m_primaryHierarchy->SetSelectedObject(row->joint);
            if (m_primaryProperties)
                m_primaryProperties->SetSelectedObject(row->joint);
            if (!additive) document.selectedKeys.clear();
        }
        else if (row && !additive) document.selectedKeys.clear();
    }
    if (document.keyPressActive)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (std::abs(mouse.x - document.keyPressX) > 3.f)
                document.keyDragging = true;
            if (document.keyDragging && !document.selectedKeys.empty())
            {
                float earliest = document.selectedKeys.front().time;
                for (const TimelineKey& key : document.selectedKeys)
                    earliest = std::min(earliest, key.time);
                document.keyDragDelta = std::max(-earliest,
                    (mouse.x - document.keyPressX) / pixelsPerSecond);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
        else
        {
            if (document.keyDragging &&
                std::abs(document.keyDragDelta) > 1e-5f &&
                !document.selectedKeys.empty())
            {
                const size_t rig = document.selectedKeys.front().rigIndex;
                auto track = std::find_if(document.tracks.begin(),
                    document.tracks.end(), [&](const auto& candidate)
                    { return candidate.index == rig; });
                if (track != document.tracks.end())
                {
                    auto before = track->sourceManager->clips;
                    auto moved = before;
                    bool valid = true;
                    for (size_t clipIndex = 0; clipIndex < moved.size(); ++clipIndex)
                    {
                        std::vector<AnimationClipEditing::KeyRef> keys;
                        for (const TimelineKey& key : document.selectedKeys)
                            if (key.rigIndex == rig &&
                                key.clipIndex == static_cast<int>(clipIndex))
                                keys.push_back({ key.nodeIndex, key.time });
                        if (!keys.empty() && !AnimationClipEditing::MoveKeys(
                                moved[clipIndex], keys,
                                document.keyDragDelta)) valid = false;
                    }
                    if (valid)
                    {
                        track->sourceManager->clips = std::move(moved);
                        if (document.rigIndex != rig)
                            FocusAnimationRig(document, rig);
                        for (TimelineKey& key : document.selectedKeys)
                            key.time += document.keyDragDelta;
                        document.playheadSeconds = std::max(0.f,
                            document.playheadSeconds + document.keyDragDelta);
                        RecordAnimationEdit(document, std::move(before));
                    }
                }
            }
            document.keyPressActive = false;
            document.keyDragging = false;
            document.keyDragDelta = 0.f;
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_C) &&
            !document.selectedKeys.empty())
            CopyAnimationKeys(document);
        if (ImGui::IsKeyPressed(ImGuiKey_V) &&
            !document.copiedKeys.empty())
            PasteAnimationKeys(document);
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        !document.selectedKeys.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
            ImGui::IsKeyPressed(ImGuiKey_Backspace)))
    {
        const size_t rig = document.selectedKeys.front().rigIndex;
        auto track = std::find_if(document.tracks.begin(),
            document.tracks.end(), [&](const auto& candidate)
            { return candidate.index == rig; });
        if (track != document.tracks.end())
        {
            auto before = track->sourceManager->clips;
            bool removed = false;
            for (const TimelineKey& key : document.selectedKeys)
                if (key.rigIndex == rig && key.clipIndex >= 0 &&
                    key.clipIndex < static_cast<int>(
                        track->sourceManager->clips.size()))
                    removed |= AnimationClipEditing::DeleteBoneKey(
                        track->sourceManager->clips[key.clipIndex],
                        key.nodeIndex, key.time);
            if (removed)
            {
                if (document.rigIndex != rig) FocusAnimationRig(document, rig);
                document.selectedKeys.clear();
                RecordAnimationEdit(document, std::move(before));
            }
        }
    }
    if (editedTrack)
    {
        if (document.rigIndex != editedTrack->index)
            FocusAnimationRig(document, editedTrack->index);
        RecordAnimationEdit(document, std::move(editBefore));
    }
    else if (refreshClipCache)
    {
        ++document.clipRevision;
        document.clipCache.clear();
        document.previewLayouts.clear();
        SampleAnimationDocument(document);
    }
    if (removeLayerFrom)
    {
        AnimationTimelineEditing::Remove(*removeLayerFrom, removeTimelineClip);
        document.selectedKeys.clear();
        document.selectedRows.clear();
        FocusAnimationRig(document, removeLayerFrom->index);
        document.clipIndex = removeLayerFrom->clipIndex;
        AnimationPlaybackChanged(document);
    }
    if (removeLibraryFrom)
        RemoveAnimationFromDocument(document, removeLibraryFrom->index, removeLibraryClip);
    const float playheadX = timeX +
        document.playheadSeconds * pixelsPerSecond;
    if (playheadX >= labelRight)
    {
        draw->AddLine({ playheadX, origin.y }, { playheadX, bottomY },
            IM_COL32(255, 225, 125, 240), 2.f);
        draw->AddRectFilled({ playheadX - 6.f, origin.y + 1.f },
            { playheadX + 6.f, origin.y + 14.f },
            IM_COL32(255, 225, 125, 255), 2.f);
        char timeLabel[32];
        std::snprintf(timeLabel, sizeof(timeLabel), "%.2f s",
            document.playheadSeconds);
        draw->AddText({ playheadX + 9.f, origin.y + 1.f },
            IM_COL32(255, 225, 125, 255), timeLabel);
    }
    if (document.playing || document.playheadDragging)
    {
        const float scroll = ImGui::GetScrollX();
        const float viewportWidth = ImGui::GetWindowWidth();
        const float markerLocalX = labelWidth +
            document.playheadSeconds * pixelsPerSecond;
        if (markerLocalX > scroll + viewportWidth - 36.f)
            ImGui::SetScrollX(markerLocalX - viewportWidth + 36.f);
        else if (markerLocalX < scroll + labelWidth + 4.f)
            ImGui::SetScrollX(std::max(0.f,
                markerLocalX - labelWidth - 4.f));
    }
    if (pendingAssignment.ready)
        AssignAnimationChannels(document, pendingAssignment.rig,
            pendingAssignment.clip, pendingAssignment.channel,
            pendingAssignment.bone);
    ImGui::EndChild();
}


}
