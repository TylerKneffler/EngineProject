#include "RigMorphControls.h"
#include "RigController.h" // legacy scene data conversion
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
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
RigMorphControls::RigMorphControls() {
    SetTypeName(COMPONENT_TYPE_NAME(RigMorphControls));
    singlecomponent = true;
}
void RigMorphControls::Start() {
    m_lastPose.clear();
    m_animatedMorphWeights.clear();
}
void RigMorphControls::SetAnimatedMorphWeights(const std::unordered_map<std::string, float>& weights) {
    m_animatedMorphWeights = weights;
}
void RigMorphControls::ApplyBoneMorphs() {
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (const auto& [node, last] : m_lastPose)
        if (Object* bone = model->ResolveNode(node);
            bone && glm::distance(bone->transform.position, last.outputPosition) < 1e-5f &&
            glm::distance(bone->transform.rotation, last.outputRotation) < 1e-5f) {
            bone->transform.position = last.inputPosition;
            bone->transform.rotation = last.inputRotation;
        }
    std::unordered_map<unsigned, LastPose> nextPose;
    const auto capture = [&](unsigned nodeIndex, Object& bone) {
        auto [found, inserted] = nextPose.try_emplace(nodeIndex);
        if (inserted) {
            found->second.inputPosition = bone.transform.position;
            found->second.inputRotation = bone.transform.rotation;
        }
    };
    for (const auto& morph : boneMorphs)
    {
        const auto animated = m_animatedMorphWeights.find(morph.name);
        const float weight = std::clamp(animated == m_animatedMorphWeights.end()
            ? morph.weight : animated->second, 0.f, 1.f);
        if (weight <= 0.f) continue;
        for (const auto& offset : morph.offsets)
        {
            Object* bone = model->ResolveNode(offset.nodeIndex);
            if (!bone) continue;
            capture(offset.nodeIndex, *bone);
            bone->transform.position += offset.translation * weight;
            bone->transform.rotation = glm::eulerAngles(glm::normalize(
                glm::quat(bone->transform.rotation) *
                glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f),
                    offset.rotation, weight)));
        }
    }
    for (auto& [node, last] : nextPose)
        if (Object* bone = model->ResolveNode(node)) {
            last.outputPosition = bone->transform.position;
            last.outputRotation = bone->transform.rotation;
        }
    m_lastPose = std::move(nextPose);
}
void RigMorphControls::RefreshPoseOutputs() {
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (auto& [node, last] : m_lastPose)
        if (Object* bone = model->ResolveNode(node)) {
            last.outputPosition = bone->transform.position;
            last.outputRotation = bone->transform.rotation;
        }
}
void RigMorphControls::ApplyImpulseMorphs() {
    if (Owner && !impulseMorphs.empty())
    {
        struct Impulses
        {
            glm::vec3 worldVelocity { 0.f };
            glm::vec3 worldTorque { 0.f };
            glm::vec3 localVelocity { 0.f };
            glm::vec3 localTorque { 0.f };
            bool stop = false;
        };
        std::unordered_map<std::string, Impulses> byBody;
        for (const auto& morph : impulseMorphs)
        {
            const auto animated = m_animatedMorphWeights.find(morph.name);
            const float weight = std::clamp(animated == m_animatedMorphWeights.end()
                ? morph.weight : animated->second, 0.f, 1.f);
            if (weight == 0.f) continue;
            for (const auto& offset : morph.offsets)
            {
                if (offset.bodyName.empty()) continue;
                auto& impulse = byBody[offset.bodyName];
                if (glm::dot(offset.velocity, offset.velocity) == 0.f &&
                    glm::dot(offset.torque, offset.torque) == 0.f)
                {
                    impulse.stop = true;
                    continue;
                }
                if (offset.local)
                {
                    impulse.localVelocity += offset.velocity * weight;
                    impulse.localTorque += offset.torque * weight;
                }
                else
                {
                    impulse.worldVelocity += offset.velocity * weight;
                    impulse.worldTorque += offset.torque * weight;
                }
            }
        }
        const float scale = glm::length(glm::vec3(
            Owner->transform.GetWorldMatrix()[0]));
        if (std::isfinite(scale) && scale > 0.f)
            for (const auto& [name, impulse] : byBody)
            {
                Object* bodyObject = Owner->FindObjectInChildrenByName(name);
                auto* body = bodyObject ? bodyObject->GetComponent<RigidBody>() : nullptr;
                if (!body || body->bodyType != "Dynamic") continue;
                if (impulse.stop)
                    body->StopMotion();
                const glm::quat rotation = WorldRotation(*bodyObject);
                const glm::vec3 velocity = impulse.worldVelocity +
                    rotation * impulse.localVelocity;
                const glm::vec3 torque = impulse.worldTorque +
                    rotation * impulse.localTorque;
                if (glm::dot(velocity, velocity) > 0.f)
                    body->AddImpulse(velocity * scale);
                if (glm::dot(torque, torque) > 0.f)
                    body->AddAngularImpulse(torque * scale * scale);
            }
    }
}
bool RigMorphControls::DrawProperties(::Engine::Editor::IEditorUi& ui) {
    bool changed = Component::DrawProperties(ui);
    struct Control {
        std::string name;
        float value = 0.f;
        std::vector<float*> weights;
        std::vector<Mesh*> meshes;
        std::vector<MaterialPropertyMorph*> materials;
    };
    std::vector<Control> controls;
    std::unordered_map<std::string, size_t> indices;
    const auto add = [&](const std::string& name, float& weight) -> Control& {
        auto [found, inserted] = indices.try_emplace(name, controls.size());
        if (inserted) {
            controls.emplace_back();
            controls.back().name = name;
            controls.back().value = weight;
        }
        controls[found->second].weights.push_back(&weight);
        return controls[found->second];
    };
    for (auto& morph : boneMorphs) add(morph.name, morph.weight);
    for (auto& morph : impulseMorphs) add(morph.name, morph.weight);
    const auto collect = [&](auto&& self, Object* object) -> void {
        if (!object) return;
        if (auto* material = object->GetComponent<MaterialPropertyMorph>())
            for (auto& item : material->morphs)
                add(item.name, item.weight).materials.push_back(material);
        if (auto* mesh = object->GetComponent<Mesh>()) {
            const auto& targets = mesh->GetMorphTargets();
            auto& weights = mesh->GetMorphWeights();
            for (size_t i = 0; i < targets.size() && i < weights.size(); ++i)
                add(targets[i].name, weights[i]).meshes.push_back(mesh);
        }
        for (auto* child : object->Children) self(self, child);
    };
    collect(collect, Owner);
    if (!controls.empty() && ui.PropertyGroupHeader("Morphs"))
        for (size_t i = 0; i < controls.size(); ++i) {
            auto& control = controls[i];
            ui.PushId(std::to_string(i).c_str());
            if (ui.DragFloat(control.name.c_str(), &control.value, .01f, 0.f, 1.f)) {
                for (float* weight : control.weights) *weight = control.value;
                for (Mesh* mesh : control.meshes) mesh->SyncMorphWeights();
                for (MaterialPropertyMorph* material : control.materials) material->MarkConfigurationDirty();
                changed = true;
            }
            ui.PopId();
        }
    if (changed) MarkConfigurationDirty();
    return changed;
}
RigMorphControls::JsonValue RigMorphControls::Serialize() const {
    RigController legacy;
    legacy.boneMorphs = boneMorphs;
    legacy.impulseMorphs = impulseMorphs;
    JsonValue value = legacy.Serialize();
    value.Set("type", JsonValue("RigMorphControls"));
    value.Erase("appends");
    value.Erase("ikChains");
    value.Erase("fixedAxes");
    value.Erase("localAxes");
    value.Erase("externalParents");
    value.Erase("physicsBones");
    value.Erase("physicsJoints");
    return value;
}
void RigMorphControls::Deserialize(const JsonValue& value) {
    RigController legacy;
    legacy.Deserialize(value);
    Component::Deserialize(value);
    boneMorphs = std::move(legacy.boneMorphs);
    impulseMorphs = std::move(legacy.impulseMorphs);
    Start();
}
}
