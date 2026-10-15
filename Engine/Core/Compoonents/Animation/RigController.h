#pragma once

#include "Core/Component.h"
#include "Core/Model/ImportedModel.h"
#include <unordered_map>
#include <vector>

namespace Engine::Components
{
class Model;
// Compatibility payload for scenes serialized before rig behavior was split
// into RigConstraints, RigMorphControls, and RigPhysicsBinding.
class RigController final : public Engine::Core::Component
{
public:
    RigController();
    std::vector<Engine::Model::ImportedAppendRule> appendRules;
    std::vector<Engine::Model::ImportedIkChain> ikChains;
    std::vector<Engine::Model::ImportedFixedAxis> fixedAxes;
    std::vector<Engine::Model::ImportedLocalAxis> localAxes;
    std::vector<Engine::Model::ImportedExternalParent> externalParents;
    std::vector<Engine::Model::ImportedPhysicsBone> physicsBones;
    std::vector<Engine::Model::ImportedPhysicsJoint> physicsJoints;
    std::vector<Engine::Model::ImportedBoneMorph> boneMorphs;
    std::vector<Engine::Model::ImportedImpulseMorph> impulseMorphs;

    void Apply();
    void PostPhysicsUpdate();
    void EnsureComponents();
    void SetAnimatedIkSwitches(const std::unordered_map<std::string, bool>& switches);
    void SetAnimatedMorphWeights(const std::unordered_map<std::string, float>& weights);
    void Start() override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    std::unordered_map<std::string, bool> m_animatedIkSwitches;
    std::unordered_map<std::string, float> m_animatedMorphWeights;
    struct ExternalRest
    {
        std::string targetName;
        glm::mat4 world { 1.f };
    };
    std::unordered_map<unsigned, ExternalRest> m_externalRests;
};
}
