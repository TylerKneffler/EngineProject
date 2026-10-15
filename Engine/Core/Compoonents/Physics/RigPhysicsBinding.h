#pragma once
#include "Core/Component.h"
#include "Core/Model/ImportedModel.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
namespace Engine::Components {
class RigidBody;
// Connects simulated bodies to model bones and describes their joints.
class RigPhysicsBinding final : public Engine::Core::Component {
public:
    RigPhysicsBinding();
    std::vector<Engine::Model::ImportedPhysicsBone> physicsBones;
    std::vector<Engine::Model::ImportedPhysicsJoint> physicsJoints;
    void Start() override;
    void Apply();
    void PostPhysicsUpdate();
    RigidBody* ResolveBody(const std::string& name) const;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
private:
    bool m_initialized = false;
    mutable uint64_t m_bodyStructureRevision = 0;
    mutable uint64_t m_bodyConfigurationRevision = 0;
    mutable std::unordered_map<std::string, RigidBody*> m_bodyLookup;
};
}
