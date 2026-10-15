#include "RigPhysicsBinding.h"
#include "Core/Compoonents/Animation/RigController.h" // legacy scene data conversion
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
namespace Engine::Components {
namespace {
using Engine::Core::Object;
glm::quat MatrixRotation(const glm::mat4& world)
{
    glm::mat3 basis(world);
    for (int axis = 0; axis < 3; ++axis)
        if (glm::length(basis[axis]) > 1e-6f)
            basis[axis] = glm::normalize(basis[axis]);
    if (glm::determinant(basis) < 0.f) basis[0] = -basis[0];
    return glm::normalize(glm::quat_cast(basis));
}

glm::quat WorldRotation(const Object& object)
{
    return MatrixRotation(object.transform.GetWorldMatrix());
}

}
RigPhysicsBinding::RigPhysicsBinding() {
    SetTypeName(COMPONENT_TYPE_NAME(RigPhysicsBinding));
    singlecomponent = true;
}
void RigPhysicsBinding::Start() {
    m_initialized = false;
    m_bodyLookup.clear();
    m_bodyStructureRevision = 0;
    m_bodyConfigurationRevision = 0;
}
RigidBody* RigPhysicsBinding::ResolveBody(const std::string& name) const {
    if (!Owner || name.empty()) return nullptr;
    const uint64_t structureRevision = Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    const uint64_t configurationRevision = GetConfigurationRevision();
    if (m_bodyStructureRevision != structureRevision ||
        m_bodyConfigurationRevision != configurationRevision) {
        m_bodyLookup.clear();
        m_bodyStructureRevision = structureRevision;
        m_bodyConfigurationRevision = configurationRevision;
    }
    if (auto found = m_bodyLookup.find(name); found != m_bodyLookup.end())
        return found->second;
    Object* bodyObject = Owner->FindObjectInChildrenByName(name);
    auto* body = bodyObject ? bodyObject->GetComponent<RigidBody>() : nullptr;
    // Missing names can resolve later while a prefab hierarchy is assembled.
    if (body) m_bodyLookup.emplace(name, body);
    return body;
}
void RigPhysicsBinding::Apply() {
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    if (Owner && !physicsBones.empty())
    {
        for (const auto& binding : physicsBones)
        {
            Object* bone = model->ResolveNode(binding.nodeIndex);
            auto* body = ResolveBody(binding.bodyName);
            if (!bone || !body) continue;
            const glm::mat4 offset = glm::translate(glm::mat4(1.f),
                binding.offsetPosition) * glm::mat4_cast(binding.offsetRotation);
            const glm::mat4 world = bone->transform.GetWorldMatrix() * offset;
            if (!m_initialized)
                body->SetWorldPose(glm::vec3(world[3]), MatrixRotation(world));
            else if (binding.mode == Engine::Model::RigidBodyMotion::SimulatedRotation)
                body->SetWorldPosition(glm::vec3(world[3]));
        }
        m_initialized = true;
    }
}

void RigPhysicsBinding::PostPhysicsUpdate()
{
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (const auto& binding : physicsBones)
    {
        Object* bone = model->ResolveNode(binding.nodeIndex);
        auto* body = ResolveBody(binding.bodyName);
        Object* bodyObject = body ? body->Owner : nullptr;
        if (!bone || !bodyObject) continue;
        const glm::mat4 offset = glm::translate(glm::mat4(1.f),
            binding.offsetPosition) * glm::mat4_cast(binding.offsetRotation);
        const glm::mat4 bodyWorld = bodyObject->transform.GetWorldMatrix();
        const glm::mat4 desiredWorld = bodyWorld * glm::inverse(offset);
        const glm::mat4 parentWorld = bone->Parent
            ? bone->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
        if (std::abs(glm::determinant(parentWorld)) < 1e-8f) continue;
        if (binding.mode == Engine::Model::RigidBodyMotion::Simulated)
            bone->transform.position = glm::vec3(glm::inverse(parentWorld) *
                glm::vec4(glm::vec3(desiredWorld[3]), 1.f));
        const glm::quat parentRotation = bone->Parent
            ? WorldRotation(*bone->Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
        bone->transform.rotation = glm::eulerAngles(glm::normalize(
            glm::inverse(parentRotation) * MatrixRotation(desiredWorld)));
    }
}

bool RigPhysicsBinding::DrawProperties(::Engine::Editor::IEditorUi& ui) {
    bool changed = Component::DrawProperties(ui);
    const std::string bodies = std::to_string(physicsBones.size());
    const std::string joints = std::to_string(physicsJoints.size());
    ui.ValueLabel("Physics-driven bones", bodies.c_str());
    ui.ValueLabel("Physics joints", joints.c_str());
    return changed;
}
RigPhysicsBinding::JsonValue RigPhysicsBinding::Serialize() const {
    RigController legacy;
    legacy.physicsBones = physicsBones;
    legacy.physicsJoints = physicsJoints;
    JsonValue value = legacy.Serialize();
    value.Set("type", JsonValue("RigPhysicsBinding"));
    value.Erase("appends");
    value.Erase("ikChains");
    value.Erase("fixedAxes");
    value.Erase("localAxes");
    value.Erase("externalParents");
    value.Erase("boneMorphs");
    value.Erase("impulseMorphs");
    return value;
}
void RigPhysicsBinding::Deserialize(const JsonValue& value) {
    RigController legacy;
    legacy.Deserialize(value);
    Component::Deserialize(value);
    physicsBones = std::move(legacy.physicsBones);
    physicsJoints = std::move(legacy.physicsJoints);
    Start();
}
}
