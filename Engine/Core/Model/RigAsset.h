#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "AnimationClipAsset.h"

namespace Engine::Scene { class Scene; }
namespace Engine::Components { class Skeleton; }
namespace Engine::Components { class Model; }
namespace Engine::Serialization { class JsonValue; }
namespace Engine::Core { class Object; }

namespace Engine::Model
{
// A stable description of one skin's bone palette and bound meshes.
// Rig-backed prefabs store their bind data here; scene snapshots may still
// carry the same data so editor undo can restore in-memory edits.
struct RigAsset
{
    struct LimbControls
    {
        int upper = -1, lower = -1, tip = -1;
    };
    struct ControlMap
    {
        int root = -1, pelvis = -1;
        // Left arm, right arm, left leg, right leg. Indices are model nodes.
        std::array<LimbControls, 4> limbs {};
    };
    struct Joint
    {
        unsigned nodeIndex = 0;
        std::string name;
        int parentNodeIndex = -1;
        glm::mat4 inverseBind { 1.f };
    };
    struct PoseJoint
    {
        unsigned nodeIndex = 0;
        glm::vec3 position { 0.f };
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 scale { 1.f };
    };
    struct Pose
    {
        std::string name;
        std::vector<PoseJoint> joints;
    };
    struct SymmetryPair
    {
        unsigned left = 0, right = 0;
    };
    unsigned skinIndex = 0;
    size_t rigIndex = 0;
    std::string prefabPath;
    std::vector<Joint> joints;
    // Model-node order and local bind transforms make a skeleton reusable
    // without loading its original prefab or model package.
    std::vector<AnimationClipAsset::BaseNode> bindNodes;
    std::vector<std::string> meshNames;
    std::vector<std::string> animationPaths;
    ControlMap controls;
    HumanoidRig::Mapping humanoid;
    std::vector<Pose> poses;
    std::vector<SymmetryPair> symmetryPairs;

    static RigAsset Capture(const Engine::Scene::Scene& scene,
        const Engine::Components::Skeleton& skeleton);
    // Prefer the explicitly mapped humanoid, then the largest armature, when
    // one imported model contains body, hair, and accessory skeletons.
    static RigAsset ForModel(Engine::Components::Model& model);
    bool Save(const std::string& path) const;
    static bool SaveBundle(const std::string& path,
        const std::vector<RigAsset>& skins);
    static std::optional<RigAsset> Load(const std::string& path,
        std::optional<unsigned> skinIndex = std::nullopt);
    static std::vector<RigAsset> LoadAll(const std::string& path);
    bool ApplySkeleton(Engine::Core::Object& owner,
        const std::string& assetPath, std::string& error) const;

private:
    Engine::Serialization::JsonValue Serialize() const;
};
}
