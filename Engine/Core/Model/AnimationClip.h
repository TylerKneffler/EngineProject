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
    std::string clipName;
    float duration = 0.f;
    bool looping = true;
    std::vector<AnimationChannel> channels;
    std::vector<VisibilityKey> visibility;
    std::vector<LimbTrack> limbs;

    static LimbKey SampleLimb(const LimbTrack& track, float time);

    Engine::Serialization::JsonValue Serialize() const;
    void Deserialize(const Engine::Serialization::JsonValue& value);
};
}
