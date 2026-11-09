#pragma once
#include "Core/ComponentReference.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Model/RigAsset.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Editor::AnimationDocumentAccess
{
inline std::string AnimationIdentity(const std::string& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = std::filesystem::path(path).lexically_normal();
    std::string result = normalized.generic_string();
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return result;
}

inline Engine::Components::Skeleton* RigAt(Engine::Scene::Scene& scene,
    size_t rigIndex)
{
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* skeleton = dynamic_cast<
                        Engine::Components::Skeleton*>(component))
                {
                    if (rigIndex == 0) return skeleton;
                    --rigIndex;
                }
    return nullptr;
}

inline Engine::Components::Model* ManagerModel(
    Engine::Components::AnimationManager& manager)
{
    if (manager.modelReference.IsAssigned())
        return Engine::Core::ResolveComponentReference<
            Engine::Components::Model>(manager.Owner,
                manager.modelReference);
    for (auto* object = manager.Owner; object; object = object->Parent)
        if (auto* model = object->GetComponent<Engine::Components::Model>())
            return model;
    return nullptr;
}

inline Engine::Components::AnimationManager* ManagerForRig(
    Engine::Scene::Scene& scene, Engine::Components::Skeleton* skeleton,
    bool* ambiguous = nullptr)
{
    if (ambiguous) *ambiguous = false;
    auto* model = skeleton ? skeleton->ResolveModel() : nullptr;
    if (!model) return nullptr;
    Engine::Components::AnimationManager* sole = nullptr;
    Engine::Components::AnimationManager* onRig = nullptr;
    Engine::Components::AnimationManager* byPath = nullptr;
    size_t count = 0;
    size_t onRigCount = 0;
    size_t pathCount = 0;
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* manager = dynamic_cast<
                        Engine::Components::AnimationManager*>(component);
                    manager && ManagerModel(*manager) == model)
                {
                    sole = manager;
                    ++count;
                    if (manager->Owner == skeleton->Owner)
                    { onRig = manager; ++onRigCount; }
                    if (!skeleton->rigPath.empty() &&
                        std::find(manager->rigPaths.begin(),
                            manager->rigPaths.end(), skeleton->rigPath) !=
                            manager->rigPaths.end())
                    { byPath = manager; ++pathCount; }
                }
    if (pathCount == 1) return byPath;
    if (pathCount > 1)
    {
        if (ambiguous) *ambiguous = true;
        return nullptr;
    }
    if (onRigCount == 1) return onRig;
    if (count == 1) return sole;
    if (ambiguous && count > 1) *ambiguous = true;
    return nullptr;
}

inline std::vector<std::string> BoundMeshNames(Engine::Scene::Scene& scene,
    Engine::Components::Skeleton* skeleton)
{
    std::vector<std::string> names;
    if (!skeleton) return names;
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* skin = dynamic_cast<
                        Engine::Components::SkinnedMesh*>(component);
                    skin && skin->ResolveSkeleton() == skeleton)
                    names.push_back(object->name);
    return names;
}

inline bool RigMatchesSkeleton(const Engine::Model::RigAsset& rig,
    Engine::Components::Skeleton& skeleton)
{
    if (rig.skinIndex != skeleton.skinIndex ||
        rig.joints.size() != skeleton.jointNodes.size()) return false;
    const auto& joints = skeleton.ResolveJoints();
    for (size_t i = 0; i < rig.joints.size(); ++i)
        if (rig.joints[i].nodeIndex != skeleton.jointNodes[i] ||
            i >= joints.size() || !joints[i] ||
            rig.joints[i].name != joints[i]->name)
            return false;
    return true;
}

inline std::string LowerText(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline Engine::Model::RigAsset::ControlMap HumanoidControls(
    const Engine::Components::Skeleton& skeleton,
    const Engine::Model::HumanoidRig::Mapping* assigned = nullptr)
{
    Engine::Model::RigAsset::ControlMap result;
    const auto& joints = skeleton.ResolveJoints();
    const auto nodeOf = [&](const Engine::Core::Object* object) -> int
    {
        const auto found = std::find(joints.begin(), joints.end(), object);
        const size_t index = static_cast<size_t>(found - joints.begin());
        return found == joints.end() || index >= skeleton.jointNodes.size()
            ? -1 : static_cast<int>(skeleton.jointNodes[index]);
    };
    const auto normalized = [](std::string name)
    {
        name = LowerText(std::move(name));
        name.erase(std::remove_if(name.begin(), name.end(),
            [](unsigned char ch) { return !std::isalnum(ch); }), name.end());
        return name;
    };
    for (auto* joint : joints)
    {
        if (!joint) continue;
        const auto name = normalized(joint->name);
        if (result.pelvis < 0 && (name.find("pelvis") != std::string::npos ||
            name.find("hips") != std::string::npos || name == "hip"))
            result.pelvis = nodeOf(joint);
        if (result.root < 0 && (name == "root" || name == "armature" ||
            name == "bip001")) result.root = nodeOf(joint);
    }
    if (result.root < 0)
        for (auto* joint : joints)
            if (joint && nodeOf(joint->Parent) < 0)
            { result.root = nodeOf(joint); break; }
    if (result.pelvis < 0) result.pelvis = result.root;
    for (size_t index = 0; index < result.limbs.size(); ++index)
    {
        const bool left = index == 0 || index == 2;
        const bool arm = index < 2;
        for (auto* joint : joints)
        {
            if (!joint || !joint->Parent || !joint->Parent->Parent) continue;
            const auto name = normalized(joint->name);
            const bool side = left
                ? name.find("left") != std::string::npos ||
                    (!name.empty() && (name.front() == 'l' || name.back() == 'l'))
                : name.find("right") != std::string::npos ||
                    (!name.empty() && (name.front() == 'r' || name.back() == 'r'));
            const bool tip = arm
                ? name.find("hand") != std::string::npos ||
                    name.find("wrist") != std::string::npos
                : name.find("foot") != std::string::npos ||
                    name.find("ankle") != std::string::npos;
            if (!side || !tip || (arm && name.find("thumb") != std::string::npos) ||
                (!arm && name.find("toe") != std::string::npos)) continue;
            const int upper = nodeOf(joint->Parent->Parent);
            const int lower = nodeOf(joint->Parent);
            const int end = nodeOf(joint);
            if (upper >= 0 && lower >= 0 && end >= 0)
            {
                result.limbs[index] = {upper, lower, end};
                break;
            }
        }
    }
    auto profile = assigned ? *assigned : Engine::Model::HumanoidRig::Mapping();
    if (!assigned)
        if (auto rig = Engine::Model::RigAsset::Load(skeleton.rigPath, skeleton.skinIndex))
            profile = rig->humanoid;
    if (profile.HasAssignments())
    {
        namespace H = Engine::Model::HumanoidRig;
        if (profile.bones[H::Root]>=0) result.root=profile.bones[H::Root];
        if (profile.bones[H::Pelvis]>=0) result.pelvis=profile.bones[H::Pelvis];
        const std::array<size_t,4> starts {H::LeftUpperArm,H::RightUpperArm,H::LeftThigh,H::RightThigh};
        for (size_t i=0;i<starts.size();++i)
        {
            const size_t first=starts[i];
            if (profile.bones[first]>=0 && profile.bones[first+1]>=0 && profile.bones[first+2]>=0)
                result.limbs[i]={profile.bones[first],profile.bones[first+1],profile.bones[first+2]};
        }
    }
    return result;
}
}
