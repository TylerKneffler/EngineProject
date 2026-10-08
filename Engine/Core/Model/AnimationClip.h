#pragma once

#include "Core/Model/AnimationData.h"
#include "Core/Serialization/Json.h"
#include <string>
#include <vector>

namespace Engine::Model
{
// Serializable clip data owned by AnimationManager. Not a scene component.
struct AnimationClip
{
    std::string clipName;
    float duration = 0.f;
    std::vector<AnimationChannel> channels;

    Engine::Serialization::JsonValue Serialize() const;
    void Deserialize(const Engine::Serialization::JsonValue& value);
};
}
