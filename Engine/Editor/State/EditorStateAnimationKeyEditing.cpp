#include "pch.h"
#include "Engine/Editor/State/EditorState.h"
#include "Core/Animation/AnimationDocumentAccess.h"
#include "Core/Animation/AnimationTimelineVisuals.h"
#include "Core/Animation/AnimationPropertySearch.h"
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
using namespace AnimationDocumentAccess;
using namespace AnimationTimelineVisuals;
using namespace AnimationPropertySearch;

void EditorState::RecordAnimationEdit(AnimationDocument& document,
    std::vector<Engine::Model::AnimationClip> before,
    std::optional<AnimationTimelineEditing::Playback> playbackBefore)
{
    ++document.clipRevision;
    document.clipCache.clear();
    document.previewLayouts.clear();
    if (m_historyLimit > 0)
    {
        if (document.undo.size() >= m_historyLimit) document.undo.pop_front();
        AnimationDocument::EditSnapshot snapshot;
        snapshot.rigIndex = document.rigIndex;
        snapshot.clips = std::move(before);
        if (playbackBefore) snapshot.playback = std::move(*playbackBefore);
        else AnimationTimelineEditing::Read(snapshot.playback, *document.sourceManager, &snapshot.clips);
        document.undo.push_back(std::move(snapshot));
    }
    document.redo.clear();
    for (const auto& track : document.tracks)
        AnimationTimelineEditing::Write(track, *track.sourceManager);
    document.dirty = document.sourceScene->SaveSnapshotToString() !=
        document.savedSnapshot || std::any_of(document.tracks.begin(),
            document.tracks.end(), [](const auto& track)
            { return track.controlsDirty || track.rigConstraintsDirty; });
    RefreshAnimationDocumentTitle(document);
    SampleAnimationDocument(document);
}

bool EditorState::CopyAnimationKeys(AnimationDocument& document)
{
    if (document.selectedKeys.empty()) return false;
    const size_t rig = document.selectedKeys.front().rigIndex;
    const auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rig](const auto& item)
        { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager) return false;
    const auto startFor = [&](int clipIndex)
    {
        if (clipIndex == track->clipIndex) return track->startSeconds;
        const auto layer = std::find_if(track->stackedClips.begin(),
            track->stackedClips.end(), [clipIndex](const auto& item)
            { return item.clipIndex == clipIndex; });
        return layer == track->stackedClips.end() ? 0.f :
            layer->startSeconds;
    };
    float firstTime = std::numeric_limits<float>::infinity();
    for (const auto& key : document.selectedKeys)
        if (key.rigIndex == rig && key.clipIndex >= 0 &&
            key.clipIndex < static_cast<int>(track->sourceManager->clips.size()))
            firstTime = std::min(firstTime,
                startFor(key.clipIndex) + key.time);
    if (!std::isfinite(firstTime)) return false;
    std::vector<Engine::Model::AnimationChannel> copied;
    for (const auto& key : document.selectedKeys)
    {
        if (key.rigIndex != rig || key.clipIndex < 0 ||
            key.clipIndex >= static_cast<int>(track->sourceManager->clips.size()))
            continue;
        auto blocks = AnimationClipEditing::CopyKeyBlocks(
            track->sourceManager->clips[key.clipIndex], key.nodeIndex,
            key.time, startFor(key.clipIndex) + key.time - firstTime);
        copied.insert(copied.end(),
            std::make_move_iterator(blocks.begin()),
            std::make_move_iterator(blocks.end()));
    }
    if (copied.empty()) return false;
    document.copiedKeys = std::move(copied);
    document.copiedKeysRig = rig;
    if (document.rigIndex != rig || document.clipIndex < 0)
        document.pasteClipIndex = document.selectedKeys.front().clipIndex;
    else
        document.pasteClipIndex = document.clipIndex;
    document.error.clear();
    return true;
}

bool EditorState::PasteAnimationKeys(AnimationDocument& document)
{
    if (document.copiedKeys.empty()) return false;
    const size_t rig = document.copiedKeysRig;
    auto track = std::find_if(document.tracks.begin(),
        document.tracks.end(), [rig](const auto& item)
        { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager ||
        document.pasteClipIndex < 0 ||
        document.pasteClipIndex >= static_cast<int>(
            track->sourceManager->clips.size()))
    {
        document.error = "Choose a destination animation before pasting keys.";
        return false;
    }
    auto before = track->sourceManager->clips;
    const float localStart = AnimationLocalTime(document, rig,
        document.pasteClipIndex);
    if (!AnimationClipEditing::PasteKeyBlocks(
            track->sourceManager->clips[document.pasteClipIndex],
            document.copiedKeys, localStart))
    {
        document.error = "Selected keys could not be pasted into this animation.";
        return false;
    }
    if (document.pasteClipIndex != track->clipIndex &&
        std::none_of(track->stackedClips.begin(), track->stackedClips.end(),
            [&](const auto& layer)
            { return layer.clipIndex == document.pasteClipIndex; }))
        track->stackedClips.push_back({ document.pasteClipIndex, 1.f });
    if (document.rigIndex != rig) FocusAnimationRig(document, rig);
    document.clipIndex = document.pasteClipIndex;
    document.selectedKeys.clear();
    for (const auto& channel : document.copiedKeys)
    {
        const AnimationDocument::TimelineKey key{ rig,
            document.pasteClipIndex, channel.nodeIndex,
            localStart + channel.times.front() };
        if (std::none_of(document.selectedKeys.begin(),
                document.selectedKeys.end(), [&](const auto& existing)
                { return existing.rigIndex == key.rigIndex &&
                    existing.clipIndex == key.clipIndex &&
                    existing.nodeIndex == key.nodeIndex &&
                    std::abs(existing.time - key.time) <= 1e-4f; }))
            document.selectedKeys.push_back(key);
    }
    document.error.clear();
    RecordAnimationEdit(document, std::move(before));
    return true;
}

bool EditorState::AddAnimationKey(AnimationDocument& document, bool exactTime)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    if (document.selectedBone < 0 || document.selectedBone >=
            static_cast<int>(joints.size()) || !joints[document.selectedBone])
        return false;
    auto* bone = joints[document.selectedBone]->GetComponent<
        Engine::Components::AnimationBone>();
    if (!bone && document.selectedBone >= static_cast<int>(
            document.poseSkeleton->jointNodes.size())) return false;
    const unsigned nodeIndex = bone ? bone->nodeIndex :
        document.poseSkeleton->jointNodes[document.selectedBone];
    auto before = document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model || !AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before keying this clip.";
        return false;
    }
    const auto& transform = joints[document.selectedBone]->transform;
    const float frameTime = AnimationLocalTime(document,
        document.rigIndex, document.clipIndex);
    const float keyTime = exactTime ? frameTime :
        AnimationClipEditing::NearestKeyTime(clip,
            nodeIndex, frameTime, .02f).value_or(frameTime);
    const bool keyed = AnimationClipEditing::UpsertBonePose(clip,
        nodeIndex, keyTime,
        transform.position, glm::quat(transform.rotation), transform.scale);
    if (keyed)
    {
        const std::string targetPath = AnimationClipMapping::NodePath(
            *model, nodeIndex);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == nodeIndex &&
                channel.targetPath.empty())
                channel.targetPath = targetPath;
        clip.duration = std::max(clip.duration, keyTime);
        RecordAnimationEdit(document, std::move(before));
    }
    return keyed;
}

bool EditorState::AddAnimationPoseKeys(AnimationDocument& document,
    const std::vector<Engine::Core::Object*>& affected)
{
    if (affected.empty() || !document.sourceManager ||
        !document.poseSkeleton || document.clipIndex < 0 ||
        document.clipIndex >= static_cast<int>(
            document.sourceManager->clips.size())) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model) return false;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    if (!AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before keying this clip.";
        return false;
    }
    auto before = document.sourceManager->clips;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto& palette = document.poseSkeleton->jointNodes;
    const float time = AnimationLocalTime(document,
        document.rigIndex, document.clipIndex);
    bool keyed = false;
    for (auto* joint : affected)
    {
        const auto found = std::find(joints.begin(), joints.end(), joint);
        const size_t index = static_cast<size_t>(found - joints.begin());
        if (found == joints.end() || index >= palette.size()) continue;
        const unsigned node = palette[index];
        const auto& transform = joint->transform;
        keyed = AnimationClipEditing::UpsertBonePose(clip, node, time,
            transform.position, glm::quat(transform.rotation),
            transform.scale) || keyed;
        const std::string path = AnimationClipMapping::NodePath(*model, node);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == node && channel.targetPath.empty())
                channel.targetPath = path;
    }
    if (keyed)
    {
        clip.duration = std::max(clip.duration, time);
        RecordAnimationEdit(document, std::move(before));
    }
    return keyed;
}

bool EditorState::AddAnimationControllerKeys(AnimationDocument& document,
    bool useExistingUndo)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size()))
        return false;
    auto before = useExistingUndo
        ? std::vector<Engine::Model::AnimationClip>{}
        : document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    const float time = AnimationLocalTime(document, document.rigIndex,
        document.clipIndex);
    const auto* model = document.poseSkeleton->ResolveModel();
    const glm::mat4 modelWorld = model && model->Owner
        ? model->Owner->transform.GetWorldMatrix() : glm::mat4(1.f);
    bool keyed = false;
    const auto* poseModel = document.poseSkeleton->ResolveModel();
    if (!poseModel) return false;
    for (auto* joint : poseModel->ResolveNodes())
    {
        const auto* bone = joint ? joint->GetComponent<
            Engine::Components::AnimationBone>() : nullptr;
        if (!bone) continue;
        auto track = std::find_if(clip.controllers.begin(),
            clip.controllers.end(), [bone](const auto& item) {
                return item.node == bone->nodeIndex;
            });
        if (!bone->poseControllerActive && !bone->posePinPosition &&
            !bone->posePinRotation && track == clip.controllers.end())
            continue;
        if (track == clip.controllers.end())
        {
            Engine::Model::AnimationClip::ControllerTrack added;
            added.node = bone->nodeIndex;
            clip.controllers.push_back(std::move(added));
            track = std::prev(clip.controllers.end());
        }
        Engine::Model::AnimationClip::ControllerKey key;
        key.time = time;
        key.space = document.controllerSpace;
        key.active = bone->poseControllerActive;
        key.fixedPosition = bone->posePinPosition;
        key.fixedRotation = bone->posePinRotation;
        key.target = Engine::Model::ControllerStoredPoint(key.space,
            modelWorld, bone->poseControllerTarget);
        key.fixedTarget = Engine::Model::ControllerStoredPoint(key.space,
            modelWorld, bone->posePinTarget);
        key.fixedRotationEuler = Engine::Model::ControllerStoredRotation(
            key.space, modelWorld, bone->posePinRotationEuler);
        key.pole = Engine::Model::ControllerStoredPoint(key.space,
            modelWorld, bone->posePinPole);
        auto existing = std::find_if(track->keys.begin(), track->keys.end(),
            [time](const auto& entry) {
                return std::abs(entry.time - time) < 1e-4f;
            });
        if (existing != track->keys.end()) *existing = key;
        else
        {
            track->keys.push_back(key);
            std::stable_sort(track->keys.begin(), track->keys.end(),
                [](const auto& a, const auto& b) {
                    return a.time < b.time;
                });
        }
        keyed = true;
    }
    if (keyed)
    {
        clip.duration = std::max(clip.duration, time);
        if (useExistingUndo)
        {
            ++document.clipRevision;
            document.clipCache.clear();
            document.previewLayouts.clear();
            document.dirty = true;
            RefreshAnimationDocumentTitle(document);
            SampleAnimationDocument(document);
        }
        else RecordAnimationEdit(document, std::move(before));
    }
    return keyed;
}

bool EditorState::AddAnimationRowKeys(AnimationDocument& document)
{
    if (document.selectedRows.empty())
        return AddAnimationKey(document, true);
    const size_t rig = document.selectedRows.front().rigIndex;
    auto track = std::find_if(document.tracks.begin(), document.tracks.end(),
        [rig](const auto& item) { return item.index == rig; });
    if (track == document.tracks.end() || !track->sourceManager ||
        !track->poseSkeleton) return false;
    auto* model = track->poseSkeleton->ResolveModel();
    if (!model) return false;
    auto& clips = track->sourceManager->clips;
    for (const auto& row : document.selectedRows)
    {
        if (row.rigIndex != rig || row.clipIndex < 0 ||
            row.clipIndex >= static_cast<int>(clips.size())) continue;
        if (!AnimationClipMapping::Inspect(clips[row.clipIndex],
                AnimationClipMapping::RigNodes(*track->poseSkeleton),
                AnimationClipMapping::ModelBones(*model)).issues.empty())
        {
            document.error = "Repair unresolved bone mappings before keying these rows.";
            return false;
        }
    }
    auto before = clips;
    bool keyed = false;
    const auto& palette = track->poseSkeleton->jointNodes;
    const auto& joints = track->poseSkeleton->ResolveJoints();
    for (const auto& row : document.selectedRows)
    {
        if (row.rigIndex != rig || row.clipIndex < 0 ||
            row.clipIndex >= static_cast<int>(clips.size())) continue;
        const auto found = std::find(palette.begin(), palette.end(), row.nodeIndex);
        const size_t index = static_cast<size_t>(found - palette.begin());
        if (found == palette.end() || index >= joints.size() || !joints[index])
            continue;
        auto& clip = clips[row.clipIndex];
        const auto& transform = joints[index]->transform;
        keyed = AnimationClipEditing::UpsertBonePose(clip, row.nodeIndex,
            AnimationLocalTime(document, row.rigIndex, row.clipIndex),
            transform.position,
            glm::quat(transform.rotation), transform.scale) || keyed;
        const std::string path = AnimationClipMapping::NodePath(
            *model, row.nodeIndex);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == row.nodeIndex && channel.targetPath.empty())
                channel.targetPath = path;
    }
    if (keyed)
    {
        if (document.rigIndex != rig) FocusAnimationRig(document, rig);
        RecordAnimationEdit(document, std::move(before));
        document.error.clear();
    }
    return keyed;
}

bool EditorState::AddAnimationIKKeys(AnimationDocument& document,
    Engine::Core::Object& upper, Engine::Core::Object& lower,
    Engine::Core::Object& tip)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    auto& clip = document.sourceManager->clips[document.clipIndex];
    if (!model || !AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before keying this clip.";
        return false;
    }
    const auto& joints = document.poseSkeleton->ResolveJoints();
    auto before = document.sourceManager->clips;
    bool keyed = false;
    for (auto* joint : { &upper, &lower, &tip })
    {
        const auto found = std::find(joints.begin(), joints.end(), joint);
        if (found == joints.end()) continue;
        const size_t palette = static_cast<size_t>(found - joints.begin());
        if (palette >= document.poseSkeleton->jointNodes.size()) continue;
        const unsigned node = document.poseSkeleton->jointNodes[palette];
        const auto& transform = joint->transform;
        keyed = AnimationClipEditing::UpsertBonePose(clip, node,
            AnimationLocalTime(document, document.rigIndex,
                document.clipIndex), transform.position,
            glm::quat(transform.rotation), transform.scale) || keyed;
        const std::string path = AnimationClipMapping::NodePath(*model, node);
        for (auto& channel : clip.channels)
            if (channel.nodeIndex == node && channel.targetPath.empty())
                channel.targetPath = path;
    }
    if (keyed)
    {
        clip.duration = std::max(clip.duration, AnimationLocalTime(document,
            document.rigIndex, document.clipIndex));
        RecordAnimationEdit(document, std::move(before));
    }
    return keyed;
}

bool EditorState::AddAnimationLimbKey(AnimationDocument& document,
    Engine::Core::Object& upper, Engine::Core::Object& lower,
    Engine::Core::Object& tip, bool matchIncoming)
{
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model || !model->Owner) return false;
    const auto& joints = document.poseSkeleton->ResolveJoints();
    const auto indexOf = [&](Engine::Core::Object& object) -> unsigned
    {
        const auto found = std::find(joints.begin(), joints.end(), &object);
        const size_t palette = static_cast<size_t>(found - joints.begin());
        return found == joints.end() || palette >= document.poseSkeleton->jointNodes.size()
            ? static_cast<unsigned>(-1) : document.poseSkeleton->jointNodes[palette];
    };
    const unsigned upperNode = indexOf(upper), lowerNode = indexOf(lower),
        tipNode = indexOf(tip);
    if (upperNode == static_cast<unsigned>(-1) ||
        lowerNode == static_cast<unsigned>(-1) ||
        tipNode == static_cast<unsigned>(-1)) return false;
    auto before = document.sourceManager->clips;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    auto found = std::find_if(clip.limbs.begin(), clip.limbs.end(),
        [&](const auto& limb) { return limb.tip == tipNode; });
    if (found == clip.limbs.end())
    {
        clip.limbs.push_back({upperNode, lowerNode, tipNode, {}});
        found = std::prev(clip.limbs.end());
    }
    const bool hadKeys = !found->keys.empty();
    const auto outgoing = hadKeys
        ? Engine::Model::AnimationClip::SampleLimb(*found,
            AnimationLocalTime(document, document.rigIndex,
                document.clipIndex))
        : Engine::Model::AnimationClip::LimbKey{};
    if (matchIncoming && document.ikBlend > 0.f)
    {
        document.ikTarget = tip.transform.GetWorldPosition();
        const glm::vec3 root = upper.transform.GetWorldPosition();
        const glm::vec3 mid = lower.transform.GetWorldPosition();
        document.ikPole = mid + mid - (root + document.ikTarget) * .5f;
        glm::mat3 basis(tip.transform.GetWorldMatrix());
        for (int axis = 0; axis < 3; ++axis)
            if (glm::length(basis[axis]) > 1e-6f)
                basis[axis] = glm::normalize(basis[axis]);
        document.ikTargetRotation = glm::eulerAngles(glm::normalize(glm::quat_cast(basis)));
    }
    if (matchIncoming && document.ikBlend <= 0.f)
    {
        for (auto* joint : {&upper, &lower, &tip})
        {
            const unsigned node = indexOf(*joint);
            const auto& transform = joint->transform;
            AnimationClipEditing::UpsertBonePose(clip, node,
                AnimationLocalTime(document, document.rigIndex,
                    document.clipIndex), transform.position,
                glm::quat(transform.rotation), transform.scale);
            const std::string path = AnimationClipMapping::NodePath(*model, node);
            for (auto& channel : clip.channels)
                if (channel.nodeIndex == node && channel.targetPath.empty())
                    channel.targetPath = path;
        }
    }
    Engine::Model::AnimationClip::LimbKey key;
    key.time = AnimationLocalTime(document, document.rigIndex,
        document.clipIndex);
    key.space = document.ikSpace;
    key.spaceNode = document.ikSpaceNode;
    key.propReference = document.ikPropReference;
    const glm::mat4 space = Engine::Model::LimbSpaceMatrix(key,
        model->Owner->transform.GetWorldMatrix(), upper, model->ResolveNodes());
    const glm::mat4 inverseSpace = glm::inverse(space);
    key.target = glm::vec3(inverseSpace * glm::vec4(document.ikTarget, 1.f));
    key.pole = glm::vec3(inverseSpace * glm::vec4(document.ikPole, 1.f));
    key.rotation = glm::normalize(glm::inverse(Engine::Model::LimbSpaceRotation(space)) *
        glm::quat(document.ikTargetRotation));
    key.blend = std::clamp(document.ikBlend, 0.f, 1.f);
    auto& keys = found->keys;
    if ((matchIncoming || !hadKeys || outgoing.space != key.space ||
            outgoing.spaceNode != key.spaceNode ||
            outgoing.propReference.objectPath != key.propReference.objectPath) &&
        key.time > .002f &&
        (!hadKeys || std::abs(outgoing.blend - key.blend) > .0001f ||
            outgoing.space != key.space || outgoing.spaceNode != key.spaceNode ||
            outgoing.propReference.objectPath != key.propReference.objectPath))
    {
        if (!hadKeys)
        {
            auto start = key;
            start.time = 0.f;
            start.blend = 0.f;
            keys.push_back(start);
        }
        auto guard = outgoing;
        guard.time = key.time - .001f;
        const auto guardAt = std::lower_bound(keys.begin(), keys.end(), guard.time,
            [](const auto& existing, float time) { return existing.time < time; });
        keys.insert(guardAt, guard);
    }
    const auto at = std::lower_bound(keys.begin(), keys.end(), key.time,
        [](const auto& existing, float time) { return existing.time < time; });
    if (at != keys.end() && std::abs(at->time - key.time) < .0001f)
        *at = key;
    else keys.insert(at, key);
    clip.duration = std::max(clip.duration, key.time);
    document.ikControlTime = -1.f;
    RecordAnimationEdit(document, std::move(before));
    return true;
}

bool EditorState::BakeAnimationLimbs(AnimationDocument& document)
{
    FrameActivity::Mark(FrameActivity::Bake);
    if (!document.sourceManager || !document.poseSkeleton ||
        document.clipIndex < 0 || document.clipIndex >=
            static_cast<int>(document.sourceManager->clips.size())) return false;
    auto& clip = document.sourceManager->clips[document.clipIndex];
    if (clip.limbs.empty() && clip.controllers.empty()) return false;
    auto* model = document.poseSkeleton->ResolveModel();
    if (!model || !model->Owner) return false;
    if (!AnimationClipMapping::Inspect(clip,
            AnimationClipMapping::RigNodes(*document.poseSkeleton),
            AnimationClipMapping::ModelBones(*model)).issues.empty())
    {
        document.error = "Repair unresolved bone mappings before baking controls.";
        return false;
    }
    const auto before = document.sourceManager->clips;
    auto baked = clip;
    const float originalTime = document.playheadSeconds;
    const auto originalTracks = document.tracks;
    const auto rootPosition = model->Owner->transform.position;
    const auto rootRotation = model->Owner->transform.rotation;
    const auto rootScale = model->Owner->transform.scale;
    for (auto& track : document.tracks)
    {
        track.selected = track.index == document.rigIndex;
        if (track.selected)
        {
            track.weight = 1.f;
            track.startSeconds = 0.f;
            track.repeat = false;
            track.repeatCount = 0;
            track.stackedClips.clear();
        }
    }
    document.previewLayouts.clear();
    const int frames = std::max(1, static_cast<int>(std::ceil(clip.duration * 30.f)));
    std::vector<float> sampleTimes;
    sampleTimes.reserve(static_cast<size_t>(frames + 1));
    for (int frame = 0; frame <= frames; ++frame)
        sampleTimes.push_back(clip.duration * frame / frames);
    for (const auto& limb : clip.limbs)
        for (const auto& key : limb.keys)
            sampleTimes.push_back(key.time);
    for (const auto& controller : clip.controllers)
        for (const auto& key : controller.keys)
        {
            sampleTimes.push_back(key.time);
            if (key.time > .001f)
                sampleTimes.push_back(key.time - .001f);
        }
    std::sort(sampleTimes.begin(), sampleTimes.end());
    sampleTimes.erase(std::unique(sampleTimes.begin(), sampleTimes.end()),
        sampleTimes.end());
    const auto& nodes = model->ResolveNodes();
    std::vector<unsigned> bakeNodes;
    if (!clip.controllers.empty())
    {
        bakeNodes = document.poseSkeleton->jointNodes;
        for (const auto& controller : clip.controllers)
            if (controller.node < nodes.size())
                for (auto* ancestor = nodes[controller.node]; ancestor;
                    ancestor = ancestor->Parent)
                {
                    const auto found = std::find(nodes.begin(), nodes.end(),
                        ancestor);
                    if (found != nodes.end())
                        bakeNodes.push_back(static_cast<unsigned>(
                            found - nodes.begin()));
                    if (ancestor == model->Owner) break;
                }
        for (unsigned node = 0; node < nodes.size(); ++node)
            if (nodes[node] == model->Owner)
            { bakeNodes.push_back(node); break; }
    }
    else
        for (const auto& limb : clip.limbs)
            for (unsigned node : {limb.upper, limb.lower, limb.tip})
                bakeNodes.push_back(node);
    std::sort(bakeNodes.begin(), bakeNodes.end());
    bakeNodes.erase(std::unique(bakeNodes.begin(), bakeNodes.end()),
        bakeNodes.end());
    for (float sampleTime : sampleTimes)
    {
        model->Owner->transform.position = rootPosition;
        model->Owner->transform.rotation = rootRotation;
        model->Owner->transform.scale = rootScale;
        document.playheadSeconds = sampleTime;
        SampleAnimationDocument(document);
        if (!clip.controllers.empty())
        {
            std::vector<Engine::Components::ChainIK::Effector> effectors;
            std::vector<Engine::Components::ChainIK::PosePreference>
                preferences;
            for (unsigned node : bakeNodes)
                if (node < nodes.size() && nodes[node])
                {
                    int depth = 0;
                    auto* ancestor = nodes[node];
                    while (ancestor && ancestor != model->Owner)
                    { ancestor = ancestor->Parent; ++depth; }
                    if (ancestor)
                        preferences.push_back({nodes[node],
                            glm::quat(nodes[node]->transform.rotation),
                            depth == 0 ? .12f : depth <= 2 ? .08f : .04f});
                }
            const glm::mat4 modelWorld = model->Owner->transform.GetWorldMatrix();
            for (const auto& track : clip.controllers)
            {
                if (track.node >= nodes.size() || !nodes[track.node]) continue;
                const auto key = Engine::Model::AnimationClip::SampleController(
                    track, sampleTime);
                if (!key.active && !key.fixedPosition &&
                    !key.fixedRotation) continue;
                Engine::Components::ChainIK::Effector effector;
                for (auto* joint = nodes[track.node]; joint;
                    joint = joint->Parent)
                {
                    effector.chain.push_back(joint);
                    if (joint == model->Owner) break;
                }
                if (effector.chain.size() < 2 ||
                    effector.chain.back() != model->Owner) continue;
                std::reverse(effector.chain.begin(), effector.chain.end());
                if (key.fixedPosition || key.fixedRotation)
                {
                    effector.target = Engine::Model::ControllerWorldPoint(
                        key.space, modelWorld, key.fixedTarget);
                    effector.positionWeight = key.fixedPosition ? 1.f : 0.f;
                    effector.rotationWeight = key.fixedRotation ? 1.f : 0.f;
                    effector.rotation = Engine::Model::ControllerWorldRotation(
                        key.space, modelWorld, key.fixedRotationEuler);
                    effector.pole = Engine::Model::ControllerWorldPoint(
                        key.space, modelWorld, key.pole);
                    effector.usePole = key.fixedPosition;
                    effector.fixed = true;
                }
                else
                {
                    effector.target = Engine::Model::ControllerWorldPoint(
                        key.space, modelWorld, key.target);
                    effector.positionWeight = .4f;
                }
                effectors.push_back(std::move(effector));
            }
            if (!effectors.empty())
                Engine::Components::ChainIK::SolveMultiple(effectors,
                    24, .001f, .35f, 45.f, &preferences);
        }
        for (unsigned node : bakeNodes)
            if (node < nodes.size() && nodes[node])
            {
                const auto& transform = nodes[node]->transform;
                AnimationClipEditing::UpsertBonePose(baked, node,
                    sampleTime, transform.position,
                    glm::quat(transform.rotation), transform.scale);
                const std::string path = AnimationClipMapping::NodePath(*model, node);
                for (auto& channel : baked.channels)
                    if (channel.nodeIndex == node && channel.targetPath.empty())
                        channel.targetPath = path;
            }
    }
    model->Owner->transform.position = rootPosition;
    model->Owner->transform.rotation = rootRotation;
    model->Owner->transform.scale = rootScale;
    document.playheadSeconds = originalTime;
    document.tracks = originalTracks;
    document.previewLayouts.clear();
    baked.limbs.clear();
    baked.controllers.clear();
    clip = std::move(baked);
    document.ikControlTime = -1.f;
    document.sampledControllerClip = -2;
    RecordAnimationEdit(document, before);
    return true;
}


}
