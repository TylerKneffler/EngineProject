#pragma once

#include "Core/Model/ImportedModel.h"
#include "MmdPoseRetargeter.h"
#include "Core/Model/AnimationPackAsset.h"
#include <string>
#include <vector>

namespace Engine::Editor
{
// Binds one VMD dance to an existing imported character prefab. The two
// animation assets use stable paths so rebinding updates them in place.
class MmdDanceAssetBinder
{
public:
    // Bake onto the prefab's neutral skeleton without changing the prefab or
    // source assets. Used when dropping motion onto a scene instance.
    static bool BuildForPrefab(const std::string& prefabPath,
        const std::string& motionPath, Engine::Model::AnimationPackAsset& pack,
        std::string& error);
    // Resolve an imported FBX through its retarget map or bind a PMX directly.
    static bool BindFromPrefab(const std::string& prefabPath,
        const std::string& vmdPath, std::string& error,
        const std::string& sourcePackPath = {});

    static bool Bind(const std::string& prefabPath,
        const std::string& fbxPath, const std::string& vmdPath,
        const std::string& profilePath, const std::string& mapPath,
        std::string& error, const std::string& sourcePackPath = {});

    static bool BindBaked(const std::string& prefabPath,
        const std::string& vmdPath, const std::string& profilePath,
        const std::string& mapPath,
        const Engine::Model::AnimationClip& body,
        const MmdPoseRetargeter::FacialBake& facial,
        std::string& error, const std::string& sourcePackPath = {},
        const std::vector<std::string>& limitations = {});
};
}
