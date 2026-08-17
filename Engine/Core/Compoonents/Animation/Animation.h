#pragma once

#include "Core/Component.h"
#include "Core/Model/AnimationData.h"
#include <string>
#include <vector>

namespace Engine::Components
{
class Animation : public Engine::Core::Component
{
public:
    using AnimationChannel = Engine::Model::AnimationChannel;

    Animation();
    std::string clipName;
    float duration = 0.f;
    std::vector<AnimationChannel> channels;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
};
}
