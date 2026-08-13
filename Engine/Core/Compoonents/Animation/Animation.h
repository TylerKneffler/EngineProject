#pragma once

#include "Core/Component.h"
#include "Core/Model/AnimationData.h"
#include <string>
#include <vector>

class Animation : public Component
{
public:
    Animation();
    std::string clipName;
    float duration = 0.f;
    std::vector<AnimationChannel> channels;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
};
