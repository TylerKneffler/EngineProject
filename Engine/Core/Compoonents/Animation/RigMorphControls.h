#pragma once
#include "Core/Component.h"
#include "Core/Model/ImportedModel.h"
#include <unordered_map>
#include <vector>
namespace Engine::Components {
// Applies named bone and rigid-body impulse morphs from authored weights or
// animation overrides.
class RigMorphControls final : public Engine::Core::Component {
public:
    RigMorphControls();
    std::vector<Engine::Model::ImportedBoneMorph> boneMorphs;
    std::vector<Engine::Model::ImportedImpulseMorph> impulseMorphs;
    void Start() override;
    void ApplyBoneMorphs();
    void RefreshPoseOutputs();
    void ApplyImpulseMorphs();
    void SetAnimatedMorphWeights(const std::unordered_map<std::string, float>& weights);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    struct LastPose {
        glm::vec3 inputPosition {0.f}, inputRotation {0.f};
        glm::vec3 outputPosition {0.f}, outputRotation {0.f};
    };
    std::unordered_map<unsigned, LastPose> m_lastPose;
    std::unordered_map<std::string, float> m_animatedMorphWeights;
};
}
