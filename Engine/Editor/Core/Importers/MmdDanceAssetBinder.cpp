#include "MmdDanceAssetBinder.h"
#include "FbxModelDecoder.h"
#include "VmdMotionDecoder.h"
#include "Core/AssetRecord.h"
#include "Core/Compoonents/Animation/ModelAnimation.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Model/RigAsset.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Engine::Editor
{
namespace fs = std::filesystem;

bool MmdDanceAssetBinder::BindFromPrefab(const std::string& prefabPath,
    const std::string& vmdPath, std::string& error,
    const std::string& sourcePackPath)
{
    const auto record = Engine::Core::AssetRecord::Load(prefabPath);
    if (!record || record->sourcePath.empty())
    { error = "Selected prefab has no source FBX metadata"; return false; }
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
    std::string& error, const std::string& sourcePackPath)
{
    const bool hasBody = !body.channels.empty() || !body.visibility.empty();
    const bool hasFace = !facial.clip.channels.empty();
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
