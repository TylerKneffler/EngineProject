#pragma once

#include "Core/AssetRecord.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Model/AnimationClipAsset.h"
#include "Core/Model/RigAsset.h"
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace Engine::Editor
{
inline void WriteImportedAnimations(Engine::Model::RigAsset& rig,
    const Engine::Components::AnimationManager& manager,
    const Engine::Components::Model& model,
    const std::filesystem::path& rigPath,
    const std::filesystem::path& prefabPath,
    const std::filesystem::path& source)
{
    const auto skeleton = Engine::Model::AnimationClipAsset::
        CaptureBaseSkeleton(model);
    for (size_t index = 0; index < manager.clips.size(); ++index)
    {
        std::string name = manager.clips[index].clipName;
        for (char& character : name)
            if (!std::isalnum(static_cast<unsigned char>(character)) &&
                character != '-' && character != '_') character = '_';
        if (name.empty()) name = "Animation";
        const auto animationPath = rigPath.parent_path() /
            (rigPath.stem().string() + "_" + name + "_" +
                std::to_string(index + 1) + ".anim");
        Engine::Model::AnimationClipAsset asset;
        asset.clip = manager.clips[index];
        asset.baseSkeleton = skeleton;
        asset.previewPrefab = prefabPath.generic_string();
        asset.previewRig = rig.rigIndex;
        if (!asset.Save(animationPath.string()))
            throw std::runtime_error("Could not save animation: " +
                animationPath.string());
        rig.animationPaths.push_back(animationPath.generic_string());
        Engine::Core::AssetRecord::Ensure(animationPath, source,
            { { "importer", std::string("model-animation") },
              { "clipName", asset.clip.clipName } });
    }
}
}
