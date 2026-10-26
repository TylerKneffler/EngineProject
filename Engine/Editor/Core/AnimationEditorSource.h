#pragma once
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializerInternal.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include <stdexcept>

namespace Engine::Editor::AnimationEditorSource
{
inline std::vector<Engine::Components::Skeleton*> Rigs(Engine::Core::Object* root)
{
    std::vector<Engine::Components::Skeleton*> result;
    const auto visit = [&](auto&& self, Engine::Core::Object* object) -> void
    {
        if (!object) return;
        for (auto* component : object->Components)
            if (auto* rig = dynamic_cast<Engine::Components::Skeleton*>(component))
                result.push_back(rig);
        for (auto* child : object->Children) self(self, child);
    };
    visit(visit, root);
    return result;
}

inline std::string Capture(Engine::Core::Object& root)
{
    using Engine::Serialization::JsonValue;
    return Engine::Serialization::AssetWriteBinary(JsonValue::MakeObject()
        .Set("version", JsonValue(1))
        .Set("objects", JsonValue::MakeArray().Push(
            Engine::Serialization::Detail::SceneObjectBehavior::SerializeObject(root, false))));
}

// Clips without a preview prefab remain editable using their saved bind
// skeleton. Older clips can display a neutral hierarchy from channel paths.
inline std::string ClipPreview(const Engine::Model::AnimationClipAsset& asset)
{
    using namespace Engine;
    auto nodes = asset.bindSkeleton.empty() ? asset.baseSkeleton : asset.bindSkeleton;
    size_t count = nodes.size();
    for (const auto& channel : asset.clip.channels)
        if (channel.nodeIndex < 100000) count = std::max(count, size_t(channel.nodeIndex) + 1);
    if (count > 100000)
        throw std::runtime_error("Animation skeleton is too large to preview.");
    count = std::max(count, size_t{1});
    nodes.resize(count);
    for (const auto& channel : asset.clip.channels)
        if (channel.nodeIndex < count && nodes[channel.nodeIndex].path.empty())
            nodes[channel.nodeIndex].path = channel.targetPath;
    std::unordered_map<std::string, size_t> byPath;
    for (size_t i = 0; i < count; ++i)
    {
        if (nodes[i].path.empty()) nodes[i].path = "Bone " + std::to_string(i);
        byPath.emplace(nodes[i].path, i);
    }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto slash = nodes[i].path.find_last_of('/');
        if (nodes[i].parent.empty() && slash != std::string::npos)
            nodes[i].parent = nodes[i].path.substr(0, slash);
        const auto parent = nodes[i].parent;
        if (!parent.empty() && !byPath.count(parent))
        {
            byPath.emplace(parent, nodes.size());
            Model::AnimationClipAsset::BaseNode helper;
            helper.path = parent;
            nodes.push_back(std::move(helper));
        }
    }
    Scene::Scene scene;
    auto* root = scene.AddObject();
    root->name = asset.clip.clipName;
    auto* model = root->AddComponent<Components::Model>();
    auto* skeleton = root->AddComponent<Components::Skeleton>();
    std::vector<Core::Object*> objects;
    for (const auto& node : nodes)
    {
        auto* object = node.path == "." ? root : scene.AddObject();
        if (object != root)
            object->name = node.path.substr(node.path.find_last_of('/') == std::string::npos
                ? 0 : node.path.find_last_of('/') + 1);
        object->transform.position = node.position;
        object->transform.rotation = node.rotation;
        object->transform.scale = node.scale;
        objects.push_back(object);
    }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        auto* object = objects[i];
        if (object != root)
        {
            auto* parent = root;
            if (const auto found = byPath.find(nodes[i].parent); found != byPath.end())
                parent = objects[found->second];
            // Corrupt metadata must not create a cycle in the preview scene.
            for (auto* ancestor = parent; ancestor; ancestor = ancestor->Parent)
                if (ancestor == object) throw std::runtime_error("Animation skeleton contains a cycle.");
            object->Parent = parent; parent->Children.push_back(object);
        }
    }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        model->BindNode(static_cast<unsigned>(i), objects[i]);
        skeleton->jointNodes.push_back(static_cast<unsigned>(i));
        skeleton->inverseBindMatrices.push_back(glm::mat4(1.f));
        auto* bone = objects[i]->AddComponent<Components::AnimationBone>();
        bone->nodeIndex = static_cast<unsigned>(i);
        bone->paletteIndex = static_cast<int>(i);
    }
    auto* manager = root->AddComponent<Components::AnimationManager>();
    manager->InstallEmbeddedClips({ asset.clip });
    manager->clip = asset.clip.clipName;
    manager->playing = false;
    return scene.SaveSnapshotToString();
}
inline std::string PackPreview(const Engine::Model::AnimationPackAsset& pack)
{
    if (pack.clips.empty()) throw std::runtime_error("Pack contains no editable clips.");
    Engine::Model::AnimationClipAsset asset;
    asset.clip = pack.clips.front();
    asset.bindSkeleton = pack.bindSkeleton;
    asset.humanoid = pack.humanoid;
    for (size_t index = 1; index < pack.clips.size(); ++index)
        asset.clip.channels.insert(asset.clip.channels.end(),
            pack.clips[index].channels.begin(), pack.clips[index].channels.end());
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::LoadFromString(scene, ClipPreview(asset)))
        throw std::runtime_error("Could not construct the animation pack skeleton.");
    auto* manager = scene.GetObjects().front()->GetComponent<Engine::Components::AnimationManager>();
    manager->InstallEmbeddedClips(pack.clips);
    manager->clip = pack.activeClip;
    manager->baseAnimationEnabled = pack.baseAnimationEnabled;
    manager->timelinePlayback = pack.timelinePlayback;
    manager->timelineTime = pack.timelineTime;
    manager->layers = pack.layers;
    return scene.SaveSnapshotToString();
}

}
