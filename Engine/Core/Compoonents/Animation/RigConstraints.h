#pragma once
#include "Core/Component.h"
#include "Core/Model/ImportedModel.h"
#include <unordered_map>
#include <vector>

namespace Engine::Components {
class Model;
// Ordered model-rig constraints evaluated before and after physics.
class RigConstraints final : public Engine::Core::Component {
public:
    RigConstraints();
    std::vector<Engine::Model::ImportedAppendRule> appendRules;
    std::vector<Engine::Model::ImportedIkChain> ikChains;
    std::vector<Engine::Model::ImportedFixedAxis> fixedAxes;
    std::vector<Engine::Model::ImportedLocalAxis> localAxes;
    std::vector<Engine::Model::ImportedExternalParent> externalParents;
    void Start() override;
    void RestorePose();
    void RefreshPoseOutputs();
    void Apply();
    void PostPhysicsUpdate();
    void SetAnimatedIkSwitches(const std::unordered_map<std::string, bool>& switches);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    void ApplyConstraints(Model& model, bool afterPhysics);
    struct DeformStep {
        int layer = 0;
        int order = 0;
        int kind = 0;
        size_t index = 0;
        bool operator==(const DeformStep& other) const {
            return layer == other.layer && order == other.order &&
                kind == other.kind && index == other.index;
        }
    };
    std::vector<DeformStep> m_stepSource;
    std::vector<DeformStep> m_sortedSteps;
    std::vector<DeformStep> m_stepScratch;
    struct LastPose {
        glm::vec3 inputPosition {0.f}, inputRotation {0.f};
        glm::vec3 outputPosition {0.f}, outputRotation {0.f};
        bool applied = false;
    };
    struct ExternalRest {
        std::string targetName;
        glm::mat4 world {1.f};
    };
    std::unordered_map<unsigned, LastPose> m_lastPose;
    std::unordered_map<unsigned, ExternalRest> m_externalRests;
    std::unordered_map<std::string, bool> m_animatedIkSwitches;
};
}
