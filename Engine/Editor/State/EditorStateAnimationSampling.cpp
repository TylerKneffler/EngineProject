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

EditorState::AnimationDocument::CachedClip& EditorState::CachedAnimationClip(
    AnimationDocument& document, AnimationDocument::RigTrack& track,
    int clipIndex)
{
    const AnimationDocument::ClipCacheKey key{
        track.sourceManager, track.poseSkeleton, clipIndex };
    if (auto found = document.clipCache.find(key);
        found != document.clipCache.end()) return found->second;
    auto& source = track.sourceManager->clips[clipIndex];
    auto* model = track.poseSkeleton->ResolveModel();
    const auto mapping = AnimationClipMapping::Inspect(source,
        AnimationClipMapping::RigNodes(*track.poseSkeleton),
        model ? AnimationClipMapping::ModelBones(*model)
            : std::vector<AnimationClipMapping::Bone>{});
    AnimationDocument::CachedClip cached;
    cached.mappingReady = mapping.issues.empty();
    cached.compatible = AnimationClipMapping::CompatibleClip(source, mapping);
    for (const auto& issue : mapping.issues)
    {
        const auto& channel = source.channels[issue.channelIndex];
        auto row = std::find_if(cached.unresolvedRows.begin(),
            cached.unresolvedRows.end(), [&](const auto& candidate)
            { return candidate.path == issue.targetPath &&
                (candidate.path.size() ||
                    candidate.nodeIndex == issue.nodeIndex); });
        if (row == cached.unresolvedRows.end())
        {
            cached.unresolvedRows.push_back({issue.targetPath,
                issue.nodeIndex, issue.channelIndex, {}});
            row = std::prev(cached.unresolvedRows.end());
        }
        row->times.insert(row->times.end(), channel.times.begin(),
            channel.times.end());
    }
    for (auto& row : cached.unresolvedRows)
    {
        std::sort(row.times.begin(), row.times.end());
        row.times.erase(std::unique(row.times.begin(), row.times.end(),
            [](float a, float b) { return std::abs(a - b) < .0001f; }),
            row.times.end());
    }
    for (const auto& channel : cached.compatible.channels)
    {
        auto& times = cached.keyTimes[channel.nodeIndex];
        times.insert(times.end(), channel.times.begin(), channel.times.end());
    }
    for (const auto& controller : cached.compatible.controllers)
    {
        auto& times = cached.keyTimes[controller.node];
        for (const auto& controllerKey : controller.keys)
            times.push_back(controllerKey.time);
    }
    for (auto& [node, times] : cached.keyTimes)
    {
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(),
            [](float a, float b) { return std::abs(a - b) < .0001f; }),
            times.end());
    }
    // Build clip-wide key groups once per revision, rather than sorting all
    // bone keys on every timeline frame.
    std::vector<std::pair<float, unsigned>> summary;
    for (const auto& [node, times] : cached.keyTimes)
        for (float time : times) summary.emplace_back(time, node);
    std::sort(summary.begin(), summary.end());
    for (const auto& [time, node] : summary)
    {
        if (cached.summaryKeys.empty() ||
            std::abs(time - cached.summaryKeys.back().time) > 1e-4f)
            cached.summaryKeys.push_back({time, {}});
        cached.summaryKeys.back().keys.emplace_back(node, time);
    }
    return document.clipCache.emplace(key, std::move(cached)).first->second;
}

void EditorState::SampleAnimationDocument(AnimationDocument& document)
{
    if (!document.poseManager || !document.poseSkeleton ||
        !document.sourceManager) return;
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        {
            track.selectedBone = document.selectedBone;
            break;
        }
    const bool layeredPreview = document.tracks.size() > 1 ||
        std::any_of(document.tracks.begin(), document.tracks.end(),
            [](const auto& track)
            { return track.clipIndex < 0 || track.weight != 1.f || track.startSeconds > 0.f ||
                track.repeat || !track.stackedClips.empty(); });
    if (layeredPreview)
    {
        for (auto& track : document.tracks)
        {
            track.poseManager->playing = false;
            track.poseManager->holdCurrentPoseWhenStopped = true;
        }
        std::unordered_set<Engine::Components::Model*> sampled;
        for (auto& leader : document.tracks)
        {
            if (!leader.selected) continue;
            auto* model = leader.poseSkeleton->ResolveModel();
            if (!model || !sampled.insert(model).second) continue;
            auto* preview = leader.poseManager;
            std::string layout = std::to_string(document.clipRevision);
            for (const auto& track : document.tracks)
                if (track.selected &&
                    track.poseSkeleton->ResolveModel() == model)
                {
                    layout += "/" + std::to_string(track.index) + ":" +
                        std::to_string(track.clipIndex);
                    for (const auto& layer : track.stackedClips)
                        layout += "," + std::to_string(layer.clipIndex);
                }
            const bool rebuild = document.previewLayouts[preview] != layout;
            if (rebuild)
            {
                preview->clips.clear();
                preview->layers.clear();
                Engine::Model::AnimationClip rest;
                rest.clipName = "__AnimationEditorRestPose__";
                rest.duration = 1.f;
                preview->clips.push_back(std::move(rest));
                preview->timelinePlayback = false;
                preview->baseAnimationEnabled = true;
                preview->clip = preview->clips.front().clipName;
                preview->time = 0.f;
            }
            size_t previewLayer = 0;
            for (auto& track : document.tracks)
            {
                if (!track.selected || track.poseSkeleton->ResolveModel() != model)
                    continue;
                for (auto* joint : track.poseSkeleton->ResolveJoints())
                    if (joint) joint->transform.ClearEditorOverride();
                const auto addLayer = [&](int clipIndex, float weight,
                    float startSeconds, bool repeat, int repeatCount,
                    size_t layerIndex)
                {
                    if (clipIndex < 0 ||
                        clipIndex >= static_cast<int>(
                            track.sourceManager->clips.size())) return;
                    if (rebuild)
                    {
                        auto clip = CachedAnimationClip(document, track,
                            clipIndex).compatible;
                        clip.clipName = "__AnimationEditorRig_" +
                            std::to_string(track.index) + "_Layer_" +
                            std::to_string(layerIndex) + "__" + clip.clipName;
                        preview->clips.push_back(std::move(clip));
                        Engine::Components::AnimationManager::Layer layer;
                        layer.clip = preview->clips.back().clipName;
                        layer.looping = false;
                        layer.nodeMask = track.poseSkeleton->jointNodes;
                        preview->layers.push_back(std::move(layer));
                    }
                    auto& layer = preview->layers[previewLayer++];
                    const auto stored = std::find_if(track.stackedClips.begin(),
                        track.stackedClips.end(), [&](const auto& item)
                        { return item.clipIndex == clipIndex; });
                    const auto& settings = stored == track.stackedClips.end()
                        ? track.baseSettings : stored->settings;
                    layer.enabled = settings.enabled;
                    layer.additive = settings.additive;
                    layer.nodeMask = settings.nodeMask;
                    const float length = std::max(.01f,
                        track.sourceManager->clips[clipIndex].duration);
                    const float elapsed = (document.playheadSeconds -
                        startSeconds) * std::abs(settings.speed);
                    const bool active = elapsed >= 0.f &&
                        (repeat ? repeatCount == 0 ||
                            elapsed < length * repeatCount :
                            elapsed <= length);
                    layer.time = repeat ? std::fmod(
                        std::max(0.f, elapsed), length) :
                        std::clamp(elapsed, 0.f, length);
                    layer.weight = active ? weight : 0.f;
                };
                addLayer(track.clipIndex, track.weight,
                    track.startSeconds, track.repeat, track.repeatCount, 0);
                for (size_t layerIndex = 0;
                    layerIndex < track.stackedClips.size(); ++layerIndex)
                    addLayer(track.stackedClips[layerIndex].clipIndex,
                        track.stackedClips[layerIndex].weight,
                        track.stackedClips[layerIndex].startSeconds,
                        track.stackedClips[layerIndex].repeat,
                        track.stackedClips[layerIndex].repeatCount,
                        layerIndex + 1);
            }
            if (rebuild) document.previewLayouts[preview] = std::move(layout);
            preview->holdCurrentPoseWhenStopped = false;
            preview->Tick(0.f);
            preview->holdCurrentPoseWhenStopped = true;
        }
        for (const auto& track : document.tracks)
        {
            if (track.selected || sampled.count(track.poseSkeleton->ResolveModel()))
                continue;
            auto* sourceModel = track.sourceSkeleton->ResolveModel();
            auto* poseModel = track.poseSkeleton->ResolveModel();
            if (!sourceModel || !poseModel) continue;
            const auto& sourceNodes = sourceModel->ResolveNodes();
            const auto& poseNodes = poseModel->ResolveNodes();
            for (size_t index = 0; index < std::min(sourceNodes.size(),
                    poseNodes.size()); ++index)
                if (sourceNodes[index] && poseNodes[index])
                {
                    poseNodes[index]->transform.ClearEditorOverride();
                    poseNodes[index]->transform.position =
                        sourceNodes[index]->transform.position;
                    poseNodes[index]->transform.rotation =
                        sourceNodes[index]->transform.rotation;
                    poseNodes[index]->transform.scale =
                        sourceNodes[index]->transform.scale;
                }
        }
        document.poseScene->ApplyPoseConstraints();
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    const int clipIndex = document.clipIndex;
    const auto& clips = document.sourceManager->clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size()))
    {
        auto* sourceSkeleton = RigAt(*document.sourceScene,
            document.rigIndex);
        auto* sourceModel = sourceSkeleton ? sourceSkeleton->ResolveModel() : nullptr;
        auto* poseModel = document.poseSkeleton->ResolveModel();
        if (sourceModel && poseModel)
        {
            const auto& sourceNodes = sourceModel->ResolveNodes();
            const auto& poseNodes = poseModel->ResolveNodes();
            for (size_t index = 0; index < std::min(sourceNodes.size(),
                    poseNodes.size()); ++index)
                if (sourceNodes[index] && poseNodes[index])
                {
                    poseNodes[index]->transform.ClearEditorOverride();
                    poseNodes[index]->transform.position =
                        sourceNodes[index]->transform.position;
                    poseNodes[index]->transform.rotation =
                        sourceNodes[index]->transform.rotation;
                    poseNodes[index]->transform.scale =
                        sourceNodes[index]->transform.scale;
                }
        }
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    for (auto* joint : document.poseSkeleton->ResolveJoints())
        if (joint) joint->transform.ClearEditorOverride();
    auto* selectedTrack = static_cast<AnimationDocument::RigTrack*>(nullptr);
    for (auto& track : document.tracks)
        if (track.index == document.rigIndex)
        { selectedTrack = &track; break; }
    if (!selectedTrack) return;
    const std::string layout = std::to_string(document.clipRevision) +
        "/single/" + std::to_string(document.rigIndex) + ":" +
        std::to_string(clipIndex);
    if (document.previewLayouts[document.poseManager] != layout)
    {
        document.poseManager->clips.clear();
        document.poseManager->layers.clear();
        document.poseManager->clips.push_back(
            CachedAnimationClip(document, *selectedTrack,
                clipIndex).compatible);
        document.previewLayouts[document.poseManager] = layout;
    }
    document.poseManager->timelinePlayback = false;
    document.poseManager->baseAnimationEnabled = true;
    document.poseManager->clip = clips[clipIndex].clipName;
    document.poseManager->time = document.playheadSeconds;
    document.poseManager->looping = false;
    document.poseManager->holdCurrentPoseWhenStopped = false;
    document.poseManager->Tick(0.f);
    document.poseManager->holdCurrentPoseWhenStopped = true;
    if (document.sampledControllerClip != clipIndex ||
        document.sampledControllerRig != document.rigIndex)
    {
        auto* sourceSkeleton = RigAt(*document.sourceScene,
            document.rigIndex);
        auto* sourceModel = sourceSkeleton
            ? sourceSkeleton->ResolveModel() : nullptr;
        auto* poseModel = document.poseSkeleton->ResolveModel();
        if (sourceModel && poseModel)
            for (auto* sourceJoint : sourceModel->ResolveNodes())
            {
                const auto* sourceBone = sourceJoint ?
                    sourceJoint->GetComponent<
                        Engine::Components::AnimationBone>() : nullptr;
                if (!sourceBone) continue;
                for (auto* poseJoint : poseModel->ResolveNodes())
                    if (auto* poseBone = poseJoint ?
                            poseJoint->GetComponent<
                                Engine::Components::AnimationBone>() : nullptr;
                        poseBone && poseBone->nodeIndex ==
                            sourceBone->nodeIndex)
                    {
                        poseBone->poseControllerActive =
                            sourceBone->poseControllerActive;
                        poseBone->poseControllerTarget =
                            sourceBone->poseControllerTarget;
                        poseBone->posePinPosition =
                            sourceBone->posePinPosition;
                        poseBone->posePinRotation =
                            sourceBone->posePinRotation;
                        poseBone->posePinTarget = sourceBone->posePinTarget;
                        poseBone->posePinRotationEuler =
                            sourceBone->posePinRotationEuler;
                        poseBone->posePinPole = sourceBone->posePinPole;
                        break;
                    }
            }
        document.sampledControllerClip = clipIndex;
        document.sampledControllerRig = document.rigIndex;
    }
    const auto& controllerClip = clips[clipIndex];
    const float controllerTime = AnimationLocalTime(document,
        document.rigIndex, clipIndex);
    const auto* poseModel = document.poseSkeleton->ResolveModel();
    if (!poseModel) return;
    const glm::mat4 modelWorld = poseModel && poseModel->Owner
        ? poseModel->Owner->transform.GetWorldMatrix() : glm::mat4(1.f);
    for (const auto& track : controllerClip.controllers)
    {
        const auto key = Engine::Model::AnimationClip::SampleController(
            track, controllerTime);
        for (auto* joint : poseModel->ResolveNodes())
            if (auto* bone = joint ? joint->GetComponent<
                    Engine::Components::AnimationBone>() : nullptr;
                bone && bone->nodeIndex == track.node)
            {
                bone->poseControllerActive = key.active;
                bone->poseControllerTarget = Engine::Model::
                    ControllerWorldPoint(key.space, modelWorld, key.target);
                bone->posePinPosition = key.fixedPosition;
                bone->posePinRotation = key.fixedRotation;
                bone->posePinTarget = Engine::Model::ControllerWorldPoint(
                    key.space, modelWorld, key.fixedTarget);
                bone->posePinRotationEuler = glm::eulerAngles(
                    Engine::Model::ControllerWorldRotation(key.space,
                        modelWorld, key.fixedRotationEuler));
                bone->posePinPole = Engine::Model::ControllerWorldPoint(
                    key.space, modelWorld, key.pole);
                break;
            }
    }
    document.poseScene->ApplyPoseConstraints();
    if (m_renderer) m_renderer->MarkDirty();
}


}
