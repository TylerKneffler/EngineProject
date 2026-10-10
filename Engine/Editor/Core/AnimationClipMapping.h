#pragma once

#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Object.h"
#include <algorithm>
#include <string>
#include <vector>

namespace Engine::Editor::AnimationClipMapping
{
struct Bone
{
    unsigned nodeIndex = 0;
    std::string path;
};

enum class IssueKind { Unidentified, Missing, Ambiguous, IndexMismatch };
struct Issue
{
    size_t channelIndex = 0;
    IssueKind kind = IssueKind::Missing;
    std::string targetPath;
    unsigned nodeIndex = 0;
};
struct Result
{
    std::vector<size_t> compatibleChannels;
    std::vector<Issue> issues;
};

inline std::string NodePath(const Engine::Components::Model& model,
    unsigned nodeIndex)
{
    const auto* node = model.ResolveNode(nodeIndex);
    if (!node || !model.Owner) return {};
    if (node == model.Owner) return ".";
    std::vector<std::string> names;
    const auto* current = node;
    for (; current && current != model.Owner;
        current = current->Parent)
        names.push_back(current->name);
    if (current != model.Owner || names.empty()) return {};
    std::string result;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        if (!result.empty()) result += '/';
        result += *it;
    }
    return result;
}

inline std::vector<Bone> RigBones(
    const Engine::Components::Skeleton& skeleton)
{
    std::vector<Bone> result;
    const auto* model = skeleton.ResolveModel();
    if (!model) return result;
    for (unsigned index : skeleton.jointNodes)
        if (auto path = NodePath(*model, index); !path.empty())
            result.push_back({ index, std::move(path) });
    return result;
}

inline std::vector<Bone> ModelBones(
    const Engine::Components::Model& model)
{
    std::vector<Bone> result;
    const auto& nodes = model.ResolveNodes();
    for (size_t index = 0; index < nodes.size(); ++index)
        if (auto path = NodePath(model, static_cast<unsigned>(index));
            !path.empty())
            result.push_back({ static_cast<unsigned>(index),
                std::move(path) });
    return result;
}

// Animated helper nodes in a joint's parent chain also affect that rig.
inline std::vector<Bone> RigNodes(
    const Engine::Components::Skeleton& skeleton)
{
    const auto* model = skeleton.ResolveModel();
    if (!model) return {};
    const auto& nodes = model->ResolveNodes();
    std::vector<Bone> result;
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        if (!nodes[index]) continue;
        bool relevant = false;
        for (unsigned jointIndex : skeleton.jointNodes)
            if (jointIndex < nodes.size() && nodes[jointIndex])
                for (auto* ancestor = nodes[jointIndex]; ancestor;
                    ancestor = ancestor->Parent)
                    if (ancestor == nodes[index])
                    { relevant = true; break; }
        if (relevant)
            if (auto path = NodePath(*model,
                    static_cast<unsigned>(index)); !path.empty())
                result.push_back({ static_cast<unsigned>(index),
                    std::move(path) });
    }
    return result;
}

inline Result Inspect(const Engine::Model::AnimationClip& clip,
    const std::vector<Bone>& rigBones,
    const std::vector<Bone>& modelBones)
{
    Result result;
    for (size_t index = 0; index < clip.channels.size(); ++index)
    {
        const auto& channel = clip.channels[index];
        const bool inRig = std::any_of(rigBones.begin(), rigBones.end(),
            [&](const Bone& bone)
            { return bone.nodeIndex == channel.nodeIndex; });
        if (channel.targetPath.empty())
        {
            if (inRig || std::none_of(modelBones.begin(), modelBones.end(),
                    [&](const Bone& bone)
                    { return bone.nodeIndex == channel.nodeIndex; }))
                result.issues.push_back({ index,
                    inRig ? IssueKind::Unidentified : IssueKind::Missing,
                    {}, channel.nodeIndex });
            continue;
        }
        const Bone* match = nullptr;
        size_t matches = 0;
        for (const Bone& bone : modelBones)
            if (bone.path == channel.targetPath)
            { match = &bone; ++matches; }
        if (matches != 1)
        {
            result.issues.push_back({ index,
                matches ? IssueKind::Ambiguous : IssueKind::Missing,
                channel.targetPath, channel.nodeIndex });
            continue;
        }
        const bool pathInRig = std::any_of(rigBones.begin(), rigBones.end(),
            [&](const Bone& bone)
            { return bone.nodeIndex == match->nodeIndex; });
        if (!pathInRig && channel.path !=
                Engine::Model::AnimationChannel::Path::Weights) continue;
        if (match->nodeIndex != channel.nodeIndex)
            result.issues.push_back({ index, IssueKind::IndexMismatch,
                channel.targetPath, channel.nodeIndex });
        else result.compatibleChannels.push_back(index);
    }
    return result;
}

inline Engine::Model::AnimationClip CompatibleClip(
    const Engine::Model::AnimationClip& source, const Result& mapping)
{
    auto result = source;
    result.channels.clear();
    for (size_t index : mapping.compatibleChannels)
        result.channels.push_back(source.channels[index]);
    return result;
}

inline bool Repair(Engine::Model::AnimationClip& clip,
    size_t channelIndex, const Bone& target)
{
    if (channelIndex >= clip.channels.size() || target.path.empty())
        return false;
    auto& channel = clip.channels[channelIndex];
    channel.nodeIndex = target.nodeIndex;
    channel.targetPath = target.path;
    return true;
}
}
