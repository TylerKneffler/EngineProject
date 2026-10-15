#include "MmdDanceAssetBinder.h"
#include "FbxModelDecoder.h"
#include "VmdMotionDecoder.h"
#include "Core/AssetRecord.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/RigMorphControls.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/RigAsset.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Editor
{
namespace fs = std::filesystem;

namespace
{
std::string PathFromRoot(const Engine::Core::Object& object,
    const Engine::Core::Object& root)
{
    std::vector<std::string> parts;
    for (auto* current = &object; current && current != &root;
        current = current->Parent)
        parts.push_back(current->name);
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

float SampleMorph(const std::vector<const VmdMorphFrame*>& frames, float time)
{
    if (frames.empty()) return 0.f;
    if (time <= frames.front()->timeSeconds) return frames.front()->weight;
    const auto later = std::upper_bound(frames.begin(), frames.end(), time,
        [](float value, const VmdMorphFrame* key)
        { return value < key->timeSeconds; });
    if (later == frames.end()) return frames.back()->weight;
    const auto* right = *later;
    const auto* left = *(later - 1);
    const float width = right->timeSeconds - left->timeSeconds;
    const float blend = width > 0.f
        ? (time - left->timeSeconds) / width : 0.f;
    return left->weight + (right->weight - left->weight) * blend;
}

float BoneBezier(float fraction, const std::array<uint8_t, 64>& bytes,
    size_t channel)
{
    const auto control = [&](size_t row)
    {
        size_t index = row * 4 + channel;
        if (row == 0 && (channel == 2 || channel == 3) &&
            bytes[2] == 99 && bytes[3] == 15)
            index = channel == 2 ? 17 : 18;
        return std::clamp(static_cast<float>(bytes[index]) / 127.f, 0.f, 1.f);
    };
    const float x1 = control(0), y1 = control(1);
    const float x2 = control(2), y2 = control(3);
    if (x1 == 0.f && y1 == 0.f && x2 == 0.f && y2 == 0.f)
        return fraction;
    const auto cubic = [](float t, float a, float b)
    {
        const float u = 1.f - t;
        return 3.f * u * u * t * a + 3.f * u * t * t * b + t * t * t;
    };
    float low = 0.f, high = 1.f;
    for (int i = 0; i < 18; ++i)
    {
        const float mid = (low + high) * 0.5f;
        if (cubic(mid, x1, x2) < fraction) low = mid;
        else high = mid;
    }
    return cubic((low + high) * 0.5f, y1, y2);
}

bool BindPmxDirect(const std::string& prefabPath, const std::string& vmdPath,
    std::string& error, const std::string& sourcePackPath)
{
    VmdMotion motion;
    if (!VmdMotionDecoder::DecodeFile(vmdPath, motion, error)) return false;
    Engine::Scene::Scene scene;
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        scene, prefabPath);
    auto* model = root ? root->GetComponent<Engine::Components::Model>() : nullptr;
    if (!model)
    { error = "PMX prefab has no model component"; return false; }

    const std::string stem = fs::path(vmdPath).stem().string();
    Engine::Model::AnimationClip body;
    body.clipName = stem + " body";
    body.duration = motion.durationSeconds;
    MmdPoseRetargeter::FacialBake facial;
    facial.clip.clipName = stem + " facial";
    facial.clip.duration = motion.durationSeconds;

    std::unordered_map<std::string, unsigned> boneByName;
    const auto& nodes = model->ResolveNodes();
    for (unsigned i = 0; i < nodes.size(); ++i)
        if (nodes[i]) boneByName.emplace(nodes[i]->name, i);
    std::unordered_map<std::string, std::vector<const VmdBoneFrame*>> boneFrames;
    for (const auto& frame : motion.bones)
        if (boneByName.count(frame.name))
            boneFrames[frame.name].push_back(&frame);
    for (auto& [name, keys] : boneFrames)
    {
        std::stable_sort(keys.begin(), keys.end(),
            [](const auto* a, const auto* b) { return a->frame < b->frame; });
        const unsigned nodeIndex = boneByName.at(name);
        const auto* target = nodes[nodeIndex];
        const std::string targetPath = PathFromRoot(*target, *root);
        Engine::Model::AnimationChannel translation, rotation;
        translation.nodeIndex = rotation.nodeIndex = nodeIndex;
        translation.targetPath = rotation.targetPath = targetPath;
        translation.path = Engine::Model::AnimationChannel::Path::Translation;
        rotation.path = Engine::Model::AnimationChannel::Path::Rotation;
        translation.valueWidth = 3;
        rotation.valueWidth = 4;
        const auto appendSample = [&](const VmdBoneFrame& from,
            const VmdBoneFrame& to, float fraction, float sampleTime)
        {
            const auto blend = [&](size_t axis)
            {
                return glm::mix(from.translation[axis], to.translation[axis],
                    BoneBezier(fraction, to.interpolation, axis));
            };
            translation.times.push_back(sampleTime);
            rotation.times.push_back(sampleTime);
            translation.values.insert(translation.values.end(), {
                target->transform.position.x + blend(0),
                target->transform.position.y + blend(1),
                target->transform.position.z - blend(2) });
            const glm::quat first(from.rotation[3], -from.rotation[0],
                -from.rotation[1], from.rotation[2]);
            const glm::quat second(to.rotation[3], -to.rotation[0],
                -to.rotation[1], to.rotation[2]);
            const glm::quat identity(1.f, 0.f, 0.f, 0.f);
            const glm::quat source = glm::normalize(glm::slerp(
                glm::dot(first, first) > 1e-12f ? glm::normalize(first) : identity,
                glm::dot(second, second) > 1e-12f ? glm::normalize(second) : identity,
                BoneBezier(fraction, to.interpolation, 3)));
            const glm::quat base(target->transform.rotation);
            const glm::quat converted = glm::normalize(base * source);
            rotation.values.insert(rotation.values.end(), { converted.x,
                converted.y, converted.z, converted.w });
        };
        if (!keys.empty()) appendSample(*keys.front(), *keys.front(), 0.f,
            keys.front()->timeSeconds);
        for (size_t i = 1; i < keys.size(); ++i)
        {
            const auto& from = *keys[i - 1];
            const auto& to = *keys[i];
            if (to.frame == from.frame) continue;
            const uint64_t quarterFrames = std::min<uint64_t>(
                static_cast<uint64_t>(to.frame - from.frame) * 4u, 4096u);
            for (uint64_t step = 1; step <= quarterFrames; ++step)
            {
                const float fraction = static_cast<float>(step) /
                    static_cast<float>(quarterFrames);
                appendSample(from, to, fraction,
                    glm::mix(from.timeSeconds, to.timeSeconds, fraction));
            }
        }
        body.channels.push_back(std::move(translation));
        body.channels.push_back(std::move(rotation));
    }
    for (const auto& key : motion.properties)
        body.visibility.push_back({ key.timeSeconds, key.visible });
    std::unordered_set<std::string> ikNames;
    if (const auto* rig = root->GetComponent<Engine::Components::RigConstraints>())
        for (const auto& chain : rig->ikChains)
            if (const auto* controller = model->ResolveNode(chain.controllerNodeIndex))
                ikNames.insert(controller->name);
    size_t unmatchedIkSwitches = 0;
    for (const auto& key : motion.properties)
        for (const auto& ik : key.ik)
            if (ikNames.count(ik.name))
                body.ikSwitches.push_back({ key.timeSeconds, ik.name, ik.enabled });
            else ++unmatchedIkSwitches;

    std::unordered_map<std::string, std::vector<const VmdMorphFrame*>> morphFrames;
    for (const auto& frame : motion.morphs)
        morphFrames[frame.name].push_back(&frame);
    for (auto& [name, keys] : morphFrames)
        std::stable_sort(keys.begin(), keys.end(),
            [](const auto* a, const auto* b) { return a->frame < b->frame; });
    std::unordered_set<std::string> matchedNames;
    std::unordered_set<std::string> componentMorphNames;
    if (const auto* rig = root->GetComponent<Engine::Components::RigMorphControls>())
    {
        for (const auto& morph : rig->boneMorphs)
            componentMorphNames.insert(morph.name);
        for (const auto& morph : rig->impulseMorphs)
            componentMorphNames.insert(morph.name);
    }
    const auto collectMaterialMorphs = [&](auto&& self,
        Engine::Core::Object* object) -> void
    {
        if (const auto* component =
            object->GetComponent<Engine::Components::MaterialPropertyMorph>())
            for (const auto& morph : component->morphs)
                componentMorphNames.insert(morph.name);
        for (auto* child : object->Children) self(self, child);
    };
    collectMaterialMorphs(collectMaterialMorphs, root);
    for (const auto& [name, keys] : morphFrames)
        if (componentMorphNames.count(name))
        {
            matchedNames.insert(name);
            auto& track = facial.clip.componentMorphs.emplace_back();
            track.name = name;
            for (const auto* key : keys)
            {
                track.times.push_back(key->timeSeconds);
                track.weights.push_back(key->weight);
            }
        }
    const auto collectMeshes = [&](auto&& self, Engine::Core::Object* object)
        -> void
    {
        if (!object) return;
        if (const auto* mesh = object->GetComponent<Engine::Components::Mesh>();
            mesh && mesh->HasMorphTargets())
        {
            const auto& targets = mesh->GetMorphTargets();
            std::vector<float> times;
            for (const auto& target : targets)
                if (const auto found = morphFrames.find(target.name);
                    found != morphFrames.end())
                {
                    matchedNames.insert(target.name);
                    ++facial.matchedTargets;
                    for (const auto* key : found->second)
                        times.push_back(key->timeSeconds);
                }
            if (!times.empty())
            {
                std::sort(times.begin(), times.end());
                times.erase(std::unique(times.begin(), times.end()), times.end());
                Engine::Model::AnimationChannel channel;
                channel.path = Engine::Model::AnimationChannel::Path::Weights;
                channel.targetMesh = PathFromRoot(*object, *root);
                channel.valueWidth = static_cast<unsigned>(targets.size());
                channel.times = std::move(times);
                for (float time : channel.times)
                    for (const auto& target : targets)
                    {
                        const auto found = morphFrames.find(target.name);
                        channel.values.push_back(found == morphFrames.end()
                            ? 0.f : SampleMorph(found->second, time));
                    }
                facial.clip.channels.push_back(std::move(channel));
            }
        }
        for (auto* child : object->Children) self(self, child);
    };
    collectMeshes(collectMeshes, root);
    facial.matchedExpressions = matchedNames.size();
    for (const auto& [name, frames] : morphFrames)
        if (!matchedNames.count(name))
            facial.unmatchedExpressions.push_back(name);
    std::sort(facial.unmatchedExpressions.begin(),
        facial.unmatchedExpressions.end());
    if (body.channels.empty() && body.visibility.empty() && body.ikSwitches.empty() &&
        facial.clip.channels.empty() && facial.clip.componentMorphs.empty())
    { error = "VMD has no bone, IK, visibility, or morph keys matching the PMX prefab"; return false; }
    std::vector<std::string> limitations;
    if (!boneFrames.empty())
        limitations.push_back("VMD Bezier bone curves are sampled at up to 120 Hz "
            "and interpolated linearly between samples");
    if (unmatchedIkSwitches)
        limitations.push_back(std::to_string(unmatchedIkSwitches) +
            " VMD IK switch keys have no matching PMX IK controller");
    if (!facial.unmatchedExpressions.empty())
        limitations.push_back(std::to_string(facial.unmatchedExpressions.size()) +
            " VMD morph names have no imported mesh, bone, or material target");
    return MmdDanceAssetBinder::BindBaked(prefabPath, vmdPath, {}, {},
        body, facial, error, sourcePackPath, limitations);
}
}

bool MmdDanceAssetBinder::BindFromPrefab(const std::string& prefabPath,
    const std::string& vmdPath, std::string& error,
    const std::string& sourcePackPath)
{
    const auto record = Engine::Core::AssetRecord::Load(prefabPath);
    if (!record || record->sourcePath.empty())
    { error = "Selected prefab has no source model metadata"; return false; }
    std::string extension = fs::path(record->sourcePath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".pmx")
        return BindPmxDirect(prefabPath, vmdPath, error, sourcePackPath);
    const fs::path fbxPath(record->sourcePath);
    if (fbxPath.extension() != ".fbx" || !fs::is_regular_file(fbxPath))
    { error = "Selected prefab source FBX is unavailable: " + fbxPath.string(); return false; }

    fs::path selectedMap;
    fs::path selectedProfile;
    try
    {
        for (const auto& entry : fs::directory_iterator(fbxPath.parent_path()))
        {
            const std::string name = entry.path().filename().string();
            constexpr const char* suffix = ".mmdmap.json";
            if (!entry.is_regular_file() || name.size() < std::char_traits<char>::length(suffix) ||
                name.compare(name.size() - std::char_traits<char>::length(suffix),
                    std::string::npos, suffix) != 0) continue;
            const auto map = Engine::Serialization::JsonParseFile(entry.path().string());
            if (fs::path(map["targetRig"]["fbx"].AsString()).filename() != fbxPath.filename())
                continue;
            const fs::path profile = entry.path().parent_path() /
                map["sourceRig"]["profile"].AsString();
            if (!fs::is_regular_file(profile)) continue;
            if (!selectedMap.empty())
            { error = "Multiple MMD retarget maps match this FBX; choose one explicitly"; return false; }
            selectedMap = entry.path();
            selectedProfile = profile;
        }
    }
    catch (const std::exception& exception)
    { error = "Could not inspect MMD retarget map: " + std::string(exception.what()); return false; }
    if (selectedMap.empty())
    { error = "No matching .mmdmap.json and rig profile beside the selected FBX"; return false; }
    return Bind(prefabPath, fbxPath.string(), vmdPath,
        selectedProfile.string(), selectedMap.string(), error, sourcePackPath);
}

bool MmdDanceAssetBinder::Bind(const std::string& prefabPath,
    const std::string& fbxPath, const std::string& vmdPath,
    const std::string& profilePath, const std::string& mapPath,
    std::string& error, const std::string& sourcePackPath)
{
    Engine::Model::ImportedModel model;
    VmdMotion motion;
    if (!FbxModelDecoder::Decode(fbxPath, model, error) ||
        !VmdMotionDecoder::DecodeFile(vmdPath, motion, error)) return false;
    if (motion.bones.empty() && motion.properties.empty() && motion.morphs.empty())
    { error = "VMD has no character body or facial tracks"; return false; }
    try
    {
        MmdPoseRetargeter retargeter;
        if (!retargeter.Initialize(model,
                Engine::Serialization::JsonParseFile(profilePath),
                Engine::Serialization::JsonParseFile(mapPath), motion, error))
            return false;
        Engine::Model::AnimationClip body;
        MmdPoseRetargeter::FacialBake facial;
        if (!motion.bones.empty() || !motion.properties.empty())
            body = retargeter.BakeBody(motion.lastFrame,
                fs::path(vmdPath).stem().string() + " body");
        if (!motion.morphs.empty())
            facial = retargeter.BakeFacial(motion.lastFrame,
                fs::path(vmdPath).stem().string() + " facial");
        return BindBaked(prefabPath, vmdPath, profilePath, mapPath,
            body, facial, error, sourcePackPath);
    }
    catch (const std::exception& e) { error = e.what(); return false; }
}

bool MmdDanceAssetBinder::BindBaked(const std::string& prefabPath,
    const std::string& vmdPath, const std::string& profilePath,
    const std::string& mapPath,
    const Engine::Model::AnimationClip& body,
    const MmdPoseRetargeter::FacialBake& facial,
    std::string& error, const std::string& sourcePackPath,
    const std::vector<std::string>& limitations)
{
    const bool hasBody = !body.channels.empty() || !body.visibility.empty() ||
        !body.ikSwitches.empty();
    const bool hasFace = !facial.clip.channels.empty() ||
        !facial.clip.componentMorphs.empty();
    if (!hasBody && !hasFace)
    { error = "MMD motion has no bindable body or facial tracks"; return false; }
    Engine::Scene::Scene scene;
    auto* root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        scene, prefabPath);
    if (!root)
    { error = "Could not load character prefab"; return false; }
    auto* model = root->GetComponent<Engine::Components::Model>();
    if (!model)
    { error = "Character prefab has no model component"; return false; }
    std::vector<std::pair<std::string, Engine::Model::RigAsset>> rigs;
    for (auto* component : root->Components)
        if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
        {
            auto rig = Engine::Model::RigAsset::Load(
                skeleton->rigPath, skeleton->skinIndex);
            if (rig) rigs.emplace_back(skeleton->rigPath, std::move(*rig));
        }
    if (rigs.empty())
    { error = "Character prefab has no saved rig"; return false; }

    const fs::path directory = fs::path(prefabPath).parent_path();
    const std::string stem = fs::path(vmdPath).stem().string();
    const fs::path bodyPath = directory / (stem + "_body.anim");
    const fs::path facePath = directory / (stem + "_facial.anim");
    const fs::path packPath = directory / (stem + ".animpack");
    const fs::path reportPath = directory / (stem + "_mmd.json");
    const auto skeleton = Engine::Model::AnimationClipAsset::CaptureBaseSkeleton(*model);
    // Release each asset's clip copy before loading rig clips and building the pack.
    if (hasBody)
    {
        Engine::Model::AnimationClipAsset asset;
        asset.clip = body;
        asset.baseSkeleton = skeleton;
        asset.previewPrefab = prefabPath;
        asset.previewRig = rigs.front().second.rigIndex;
        if (!asset.Save(bodyPath.string()))
        { error = "Could not save MMD body animation: " + bodyPath.string(); return false; }
    }
    if (hasFace)
    {
        Engine::Model::AnimationClipAsset asset;
        asset.clip = facial.clip;
        asset.baseSkeleton = skeleton;
        asset.previewPrefab = prefabPath;
        asset.previewRig = rigs.front().second.rigIndex;
        if (!asset.Save(facePath.string()))
        { error = "Could not save MMD facial animation: " + facePath.string(); return false; }
    }

    auto report = Engine::Serialization::JsonValue::MakeObject()
        .Set("sourceVmd", Engine::Serialization::JsonValue(vmdPath))
        .Set("rigProfile", Engine::Serialization::JsonValue(profilePath))
        .Set("retargetMap", Engine::Serialization::JsonValue(mapPath))
        .Set("bodyClip", Engine::Serialization::JsonValue(
            hasBody ? bodyPath.generic_string() : std::string()))
        .Set("facialClip", Engine::Serialization::JsonValue(
            hasFace ? facePath.generic_string() : std::string()))
        .Set("animationPack", Engine::Serialization::JsonValue(
            packPath.generic_string()))
        .Set("matchedExpressions", Engine::Serialization::JsonValue(
            static_cast<int>(facial.matchedExpressions)))
        .Set("matchedTargets", Engine::Serialization::JsonValue(
            static_cast<int>(facial.matchedTargets)));
    auto unmatched = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& name : facial.unmatchedExpressions)
        unmatched.Push(Engine::Serialization::JsonValue(name));
    report.Set("unmatchedExpressions", std::move(unmatched));
    auto warnings = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& limitation : limitations)
        warnings.Push(Engine::Serialization::JsonValue(limitation));
    report.Set("limitations", std::move(warnings));
    {
        std::ofstream file(reportPath, std::ios::binary | std::ios::trunc);
        if (!file || !(file << Engine::Serialization::JsonWrite(report) << '\n'))
        { error = "Could not save MMD mapping report"; return false; }
    }

    for (auto& [path, rig] : rigs)
    {
        for (const auto& clipPath : { hasBody ? bodyPath.generic_string() : std::string(),
                                      hasFace ? facePath.generic_string() : std::string() })
        {
            if (clipPath.empty()) continue;
            if (std::find(rig.animationPaths.begin(), rig.animationPaths.end(),
                    clipPath) == rig.animationPaths.end())
                rig.animationPaths.push_back(clipPath);
        }
        if (!rig.Save(path))
        { error = "Could not update character rig: " + path; return false; }
    }
    auto* manager = root->GetComponent<Engine::Components::AnimationManager>();
    if (!manager) manager = root->AddComponent<Engine::Components::AnimationManager>();
    for (const auto& [path, rig] : rigs)
        if (std::find(manager->rigPaths.begin(), manager->rigPaths.end(), path) ==
            manager->rigPaths.end()) manager->rigPaths.push_back(path);
    manager->RefreshRigClips();
    if (hasBody)
    {
        manager->clip = body.clipName;
        manager->playing = true;
        manager->time = 0.f;
    }
    if (hasFace)
    {
        auto layer = std::find_if(manager->layers.begin(), manager->layers.end(),
            [&](const auto& item) { return item.clip == facial.clip.clipName; });
        if (layer == manager->layers.end())
        {
            manager->layers.emplace_back();
            layer = std::prev(manager->layers.end());
        }
        layer->clip = facial.clip.clipName;
        layer->enabled = true;
        layer->weight = 1.f;
        layer->time = 0.f;
        layer->speed = 1.f;
    }
    Engine::Model::AnimationPackAsset pack;
    if (!sourcePackPath.empty())
    {
        auto sourcePack = Engine::Model::AnimationPackAsset::Load(
            sourcePackPath, error);
        if (!sourcePack)
        { error = "Could not load VMD source tracks: " + error; return false; }
        pack.sourceTracks = std::move(sourcePack->sourceTracks);
    }
    pack.activeClip = hasBody ? body.clipName : facial.clip.clipName;
    if (hasBody) pack.clips.push_back(body);
    if (hasFace)
    {
        pack.clips.push_back(facial.clip);
        if (hasBody)
            pack.layers.push_back(*std::find_if(manager->layers.begin(),
                manager->layers.end(), [&](const auto& item)
                { return item.clip == facial.clip.clipName; }));
    }
    if (!pack.Save(packPath.string(), error))
    { error = "Could not save MMD animation pack: " + error; return false; }
    if (!Engine::Serialization::SceneSerializer::SavePrefab(*root, prefabPath))
    { error = "Could not update character prefab"; return false; }
    if (hasBody)
        Engine::Core::AssetRecord::Ensure(bodyPath, vmdPath,
            { { "importer", std::string("mmd-body") }, { "map", mapPath } });
    if (hasFace)
        Engine::Core::AssetRecord::Ensure(facePath, vmdPath,
            { { "importer", std::string("mmd-facial") }, { "map", mapPath } });
    Engine::Core::AssetRecord::Ensure(reportPath, vmdPath,
        { { "importer", std::string("mmd-report") } });
    Engine::Core::AssetRecord::Ensure(packPath, vmdPath,
        { { "importer", std::string("mmd-animation-pack") }, { "map", mapPath } });
    return true;
}
}
