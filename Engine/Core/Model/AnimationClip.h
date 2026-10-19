#pragma once

#include "Core/Model/AnimationData.h"
#include "Core/ComponentReference.h"
#include "Core/Serialization/Json.h"
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Model
{
// Serializable clip data owned by AnimationManager. Not a scene component.
struct AnimationClip
{
    struct VisibilityKey
    {
        float time = 0.f;
        bool visible = true;
    };
    struct IkSwitchKey
    {
        float time = 0.f;
        std::string name;
        bool enabled = true;
    };
    struct ComponentMorphTrack
    {
        std::string name;
        std::vector<float> times;
        std::vector<float> weights;
    };
    struct LimbKey
    {
        enum class Space { Body, Local, World, Prop };
        float time = 0.f;
        glm::vec3 target { 0.f }; // In the selected control space.
        glm::vec3 pole { 0.f };
        glm::quat rotation { 1.f, 0.f, 0.f, 0.f };
        float blend = 0.f;
        Space space = Space::Body;
        int spaceNode = -1; // Model node used by Prop.
        Engine::Core::ComponentReference propReference { "Transform" };
    };
    struct LimbTrack
    {
        unsigned upper = 0, lower = 0, tip = 0;
        std::vector<LimbKey> keys;
    };
    struct ControllerKey
    {
        float time = 0.f;
        bool active = false;
        bool fixedPosition = false;
        bool fixedRotation = false;
        glm::vec3 target{0.f};
        glm::vec3 fixedTarget{0.f};
        glm::vec3 fixedRotationEuler{0.f};
        glm::vec3 pole{0.f};
    };
    struct ControllerTrack
    {
        unsigned node = 0;
        std::vector<ControllerKey> keys;
    };
    std::string clipName;
    float duration = 0.f;
    bool looping = true;
    std::vector<AnimationChannel> channels;
    std::vector<VisibilityKey> visibility;
    std::vector<IkSwitchKey> ikSwitches;
    std::vector<ComponentMorphTrack> componentMorphs;
    std::vector<LimbTrack> limbs;
    std::vector<ControllerTrack> controllers;

    static LimbKey SampleLimb(const LimbTrack& track, float time);
    static ControllerKey SampleController(const ControllerTrack& track,
        float time);

    Engine::Serialization::JsonValue Serialize() const;
    void Deserialize(const Engine::Serialization::JsonValue& value);
};
}
