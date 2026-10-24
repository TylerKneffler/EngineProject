#pragma once

#include "Core/Serialization/Json.h"
#include <array>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Model::HumanoidRig
{
enum Role : size_t
{
    Root, Pelvis, Spine, Chest, UpperChest, Neck, Head,
    LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
    RightShoulder, RightUpperArm, RightLowerArm, RightHand,
    LeftThigh, LeftCalf, LeftFoot, LeftToe,
    RightThigh, RightCalf, RightFoot, RightToe, Count
};
inline constexpr std::array<const char*, Count> Keys {
    "root", "pelvis", "spine1", "spine2", "spine3", "neck", "head",
    "lclavicle", "lupperarm", "llowerarm", "lhand",
    "rclavicle", "rupperarm", "rlowerarm", "rhand",
    "lthigh", "lcalf", "lfoot", "ltoe", "rthigh", "rcalf", "rfoot", "rtoe"
};
inline constexpr std::array<const char*, Count> Labels {
    "Root", "Pelvis", "Spine", "Chest", "Upper chest", "Neck", "Head",
    "Left shoulder", "Left upper arm", "Left elbow", "Left hand",
    "Right shoulder", "Right upper arm", "Right elbow", "Right hand",
    "Left thigh", "Left knee", "Left foot", "Left toe",
    "Right thigh", "Right knee", "Right foot", "Right toe"
};

struct Mapping
{
    // Indices refer to Model nodes, never to display names or palette order.
    std::array<int, Count> bones;
    int upAxis = 1;
    int forwardAxis = 2;
    bool leftPositive = true;
    Mapping() { bones.fill(-1); }
    bool HasAssignments() const;
    Engine::Serialization::JsonValue Serialize() const;
    static Mapping Deserialize(const Engine::Serialization::JsonValue& value);
};

struct Node
{
    int parent = -1;
    glm::vec3 position{}; // Rest position in model/world coordinates.
    bool joint = true;
};
struct Detection
{
    Mapping mapping;
    std::array<float, Count> confidence{};
    std::vector<std::string> issues;
};

// Names deliberately do not participate. A/T-pose geometry and ancestry
// identify the major chains; indistinguishable chains remain unassigned.
Detection Detect(const std::vector<Node>& nodes, const Mapping& settings = {});
std::vector<std::string> Validate(const Mapping& mapping, const std::vector<Node>& nodes);

// BaseNode-like inputs keep this independent of animation/rig asset headers.
struct RestTransform
{
    std::string path, parent;
    glm::vec3 position{}, rotation{}, scale{1.f};
};
std::vector<Node> BuildNodes(const std::vector<RestTransform>& transforms,
    const std::vector<unsigned>& joints = {});
template<class T>
std::vector<Node> Nodes(const std::vector<T>& transforms,
    const std::vector<unsigned>& joints = {})
{
    std::vector<RestTransform> input;
    input.reserve(transforms.size());
    for (const auto& node : transforms)
        input.push_back({node.path, node.parent, node.position, node.rotation, node.scale});
    return BuildNodes(input, joints);
}
}
