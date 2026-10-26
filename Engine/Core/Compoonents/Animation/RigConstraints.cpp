#include "RigConstraints.h"
#include "RigController.h" // legacy scene data conversion
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Components
{
namespace
{
using Value = Engine::Serialization::JsonValue;
using Engine::Core::Object;

Value Vec3Value(const glm::vec3& vector)
{
    return Value::MakeArray().Push(Value(vector.x)).Push(Value(vector.y))
        .Push(Value(vector.z));
}

glm::vec3 ReadVec3(const Value& value)
{
    if (value.ArraySize() != 3) return glm::vec3(0.f);
    return { value.ArrayAt(0).AsFloat(), value.ArrayAt(1).AsFloat(),
        value.ArrayAt(2).AsFloat() };
}

Value QuaternionValue(const glm::quat& rotation)
{
    return Value::MakeArray().Push(Value(rotation.x)).Push(Value(rotation.y))
        .Push(Value(rotation.z)).Push(Value(rotation.w));
}

glm::quat ReadQuaternion(const Value& value)
{
    if (value.ArraySize() != 4) return { 1.f, 0.f, 0.f, 0.f };
    return glm::normalize(glm::quat(value.ArrayAt(3).AsFloat(),
        value.ArrayAt(0).AsFloat(), value.ArrayAt(1).AsFloat(),
        value.ArrayAt(2).AsFloat()));
}

glm::quat ConstrainToAxis(const glm::quat& rotation, const glm::vec3& axis)
{
    const glm::vec3 direction = glm::normalize(axis);
    const glm::vec3 projected = direction *
        glm::dot(glm::vec3(rotation.x, rotation.y, rotation.z), direction);
    const glm::quat twist(rotation.w, projected.x, projected.y, projected.z);
    return glm::dot(twist, twist) > 1e-12f
        ? glm::normalize(twist) : glm::quat(1.f, 0.f, 0.f, 0.f);
}

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

void SolveIk(const Engine::Model::ImportedIkChain& chain, const Model& model)
{
    if (!chain.enabled) return;
    const auto& nodes = model.ResolveNodes();
    const auto nodeAt = [&](unsigned index) -> Object*
    { return index < nodes.size() ? nodes[index] : nullptr; };
    Object* controller = nodeAt(chain.controllerNodeIndex);
    Object* effector = nodeAt(chain.effectorNodeIndex);
    if (!controller || !effector || chain.links.empty()) return;
    const float maxAngle = std::clamp(chain.angleLimit, 0.f, 3.14159265f);
    if (maxAngle <= 0.f) return;
    const int passes = std::clamp(chain.iterations, 0, 128);
    for (int pass = 0; pass < passes; ++pass)
    {
        const glm::vec3 goal = controller->transform.GetWorldPosition();
        if (glm::distance(effector->transform.GetWorldPosition(), goal) < .001f)
            break;
        for (const auto& link : chain.links)
        {
            Object* joint = nodeAt(link.nodeIndex);
            if (!joint || joint == controller || joint == effector) continue;
            const glm::vec3 pivot = joint->transform.GetWorldPosition();
            const glm::vec3 tip = effector->transform.GetWorldPosition() - pivot;
            const glm::vec3 target = goal - pivot;
            if (glm::dot(tip, tip) < 1e-12f ||
                glm::dot(target, target) < 1e-12f) continue;
            const glm::vec3 from = glm::normalize(tip);
            const glm::vec3 to = glm::normalize(target);
            const glm::quat parentRotation = joint->Parent
                ? WorldRotation(*joint->Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
            glm::vec3 axis;
            float angle;
            if (link.fixedAxis && glm::dot(link.axis, link.axis) > 1e-12f)
            {
                axis = glm::normalize(parentRotation * link.axis);
                const glm::vec3 fromPlane = from - axis * glm::dot(from, axis);
                const glm::vec3 toPlane = to - axis * glm::dot(to, axis);
                if (glm::dot(fromPlane, fromPlane) < 1e-12f ||
                    glm::dot(toPlane, toPlane) < 1e-12f) continue;
                const glm::vec3 projectedFrom = glm::normalize(fromPlane);
                const glm::vec3 projectedTo = glm::normalize(toPlane);
                angle = std::atan2(glm::dot(axis,
                    glm::cross(projectedFrom, projectedTo)),
                    glm::dot(projectedFrom, projectedTo));
                angle = std::clamp(angle, -maxAngle, maxAngle);
            }
            else
            {
                axis = glm::cross(from, to);
                if (glm::dot(axis, axis) < 1e-12f) continue;
                axis = glm::normalize(axis);
                angle = std::min(maxAngle,
                    std::acos(std::clamp(glm::dot(from, to), -1.f, 1.f)));
            }
            const glm::quat delta = glm::angleAxis(angle, axis);
            const glm::quat local = glm::normalize(glm::inverse(parentRotation) *
                delta * WorldRotation(*joint));
            glm::vec3 euler = glm::eulerAngles(local);
            if (link.limited)
                euler = glm::clamp(euler, link.minimum, link.maximum);
            joint->transform.rotation = euler;
        }
    }
}
}

RigConstraints::RigConstraints()
{
    SetTypeName(COMPONENT_TYPE_NAME(RigConstraints));
    singlecomponent = true;
}

void RigConstraints::Start()
{
    m_lastPose.clear();
    m_animatedIkSwitches.clear();
    m_externalRests.clear();
    m_stepSource.clear();
    m_sortedSteps.clear();
    m_stepScratch.clear();
}

void RigConstraints::SetAnimatedIkSwitches(
    const std::unordered_map<std::string, bool>& switches)
{
    m_animatedIkSwitches = switches;
}

void RigConstraints::RestorePose()
{
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (const auto& [node, last] : m_lastPose)
        if (Object* bone = model->ResolveNode(node); bone && last.applied &&
            glm::distance(bone->transform.position, last.outputPosition) < 1e-5f &&
            glm::distance(bone->transform.rotation, last.outputRotation) < 1e-5f)
        {
            bone->transform.position = last.inputPosition;
            bone->transform.rotation = last.inputRotation;
        }
}

void RigConstraints::RefreshPoseOutputs()
{
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (auto& [node, last] : m_lastPose)
        if (Object* bone = model->ResolveNode(node))
        {
            last.outputPosition = bone->transform.position;
            last.outputRotation = bone->transform.rotation;
        }
}

void RigConstraints::Apply()
{
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    std::unordered_map<unsigned, LastPose> nextPose;
    const auto capture = [&](unsigned nodeIndex, Object& bone)
    {
        auto [found, inserted] = nextPose.try_emplace(nodeIndex);
        if (inserted)
        {
            found->second.inputPosition = bone.transform.position;
            found->second.inputRotation = bone.transform.rotation;
        }
    };
    for (const auto& local : localAxes)
    {
        Object* bone = model->ResolveNode(local.nodeIndex);
        if (!bone || glm::dot(local.x, local.x) < 1e-12f) continue;
        const glm::vec3 x = glm::normalize(local.x);
        const glm::vec3 zHint = local.z - x * glm::dot(local.z, x);
        if (glm::dot(zHint, zHint) < 1e-12f) continue;
        const glm::vec3 z = glm::normalize(zHint);
        const glm::vec3 y = glm::normalize(glm::cross(z, x));
        const glm::quat basis = glm::normalize(glm::quat_cast(glm::mat3(
            x, y, z)));
        capture(local.nodeIndex, *bone);
        bone->transform.position = local.restTranslation +
            basis * (bone->transform.position - local.restTranslation);
        bone->transform.rotation = glm::eulerAngles(glm::normalize(
            basis * glm::quat(bone->transform.rotation) * glm::inverse(basis)));
    }
    for (const auto& fixed : fixedAxes)
    {
        Object* bone = model->ResolveNode(fixed.nodeIndex);
        if (!bone || glm::dot(fixed.axis, fixed.axis) < 1e-12f) continue;
        capture(fixed.nodeIndex, *bone);
        bone->transform.rotation = glm::eulerAngles(ConstrainToAxis(
            glm::quat(bone->transform.rotation), fixed.axis));
    }
    m_lastPose = std::move(nextPose);
    ApplyConstraints(*model, false);
}

void RigConstraints::ApplyConstraints(Model& modelValue, bool afterPhysics)
{
    Model* model = &modelValue;
    const auto capture = [&](unsigned nodeIndex, Object& bone)
    {
        auto [found, inserted] = m_lastPose.try_emplace(nodeIndex);
        if (inserted)
        {
            found->second.inputPosition = bone.transform.position;
            found->second.inputRotation = bone.transform.rotation;
        }
    };
    m_stepScratch.clear();
    m_stepScratch.reserve(appendRules.size() + ikChains.size() +
        externalParents.size());
    for (size_t index = 0; index < appendRules.size(); ++index)
    {
        const auto& rule = appendRules[index];
        m_stepScratch.push_back({ rule.deformLayer,
            rule.deformOrder < 0 ? static_cast<int>(index) :
                rule.deformOrder, 0, index });
    }
    for (size_t index = 0; index < ikChains.size(); ++index)
    {
        const auto& chain = ikChains[index];
        m_stepScratch.push_back({ chain.deformLayer,
            chain.deformOrder < 0 ?
                static_cast<int>(index) :
                chain.deformOrder, 1, index });
    }
    for (size_t index = 0; index < externalParents.size(); ++index)
    {
        const auto& binding = externalParents[index];
        m_stepScratch.push_back({ binding.deformLayer,
            binding.deformOrder < 0 ? static_cast<int>(index) :
                binding.deformOrder, 2, index });
    }
    if (m_stepScratch != m_stepSource)
    {
        m_stepSource = m_stepScratch;
        m_sortedSteps = m_stepScratch;
        std::stable_sort(m_sortedSteps.begin(), m_sortedSteps.end(),
            [](const DeformStep& a, const DeformStep& b)
            {
                if (a.layer != b.layer) return a.layer < b.layer;
                if (a.order != b.order) return a.order < b.order;
                return a.kind < b.kind;
            });
    }
    for (const auto& step : m_sortedSteps)
    {
        if ((step.kind == 0 && appendRules[step.index].afterPhysics != afterPhysics) ||
            (step.kind == 1 && ikChains[step.index].afterPhysics != afterPhysics) ||
            (step.kind == 2 && externalParents[step.index].afterPhysics != afterPhysics))
            continue;
        if (step.kind == 1)
        {
            const auto& chain = ikChains[step.index];
            auto* controller = model->ResolveNode(chain.controllerNodeIndex);
            const auto animated = controller
                ? m_animatedIkSwitches.find(controller->name) :
                    m_animatedIkSwitches.end();
            if (animated != m_animatedIkSwitches.end() && !animated->second)
                continue;
            for (const auto& link : chain.links)
                if (Object* bone = model->ResolveNode(link.nodeIndex))
                    capture(link.nodeIndex, *bone);
            if (animated != m_animatedIkSwitches.end() && animated->second &&
                !chain.enabled)
            {
                auto enabledChain = chain;
                enabledChain.enabled = true;
                SolveIk(enabledChain, *model);
            }
            else SolveIk(chain, *model);
            continue;
        }
        if (step.kind == 2)
        {
            const auto& binding = externalParents[step.index];
            if (binding.targetName.empty() || !Owner || !Owner->GetScene())
                continue;
            Object* bone = model->ResolveNode(binding.nodeIndex);
            Object* target = Owner->GetScene()->FindObjectByName(
                binding.targetName);
            if (!bone || !target || target == bone) continue;
            bool descendant = false;
            for (Object* parent = target->Parent; parent; parent = parent->Parent)
                if (parent == bone) { descendant = true; break; }
            if (descendant) continue;
            capture(binding.nodeIndex, *bone);
            const glm::mat4 targetWorld = target->transform.GetWorldMatrix();
            auto& rest = m_externalRests[binding.nodeIndex];
            if (rest.targetName != binding.targetName)
            {
                rest.targetName = binding.targetName;
                rest.world = targetWorld;
            }
            if (std::abs(glm::determinant(rest.world)) < 1e-8f) continue;
            const glm::mat4 delta = targetWorld * glm::inverse(rest.world);
            const glm::vec3 worldPosition = glm::vec3(delta * glm::vec4(
                bone->transform.GetWorldPosition(), 1.f));
            const glm::quat worldRotation = glm::normalize(
                MatrixRotation(targetWorld) * glm::inverse(
                    MatrixRotation(rest.world)) * WorldRotation(*bone));
            const glm::mat4 parentWorld = bone->Parent
                ? bone->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
            if (std::abs(glm::determinant(parentWorld)) < 1e-8f) continue;
            bone->transform.position = glm::vec3(glm::inverse(parentWorld) *
                glm::vec4(worldPosition, 1.f));
            const glm::quat parentRotation = bone->Parent
                ? WorldRotation(*bone->Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
            bone->transform.rotation = glm::eulerAngles(glm::normalize(
                glm::inverse(parentRotation) * worldRotation));
            continue;
        }
        const auto& rule = appendRules[step.index];
        Object* driven = model->ResolveNode(rule.nodeIndex);
        Object* source = model->ResolveNode(rule.sourceNodeIndex);
        if (!driven || !source || driven == source) continue;
        capture(rule.nodeIndex, *driven);
        auto& transform = driven->transform;
        if (rule.translation)
            transform.position += (source->transform.position -
                rule.sourceRestTranslation) * rule.influence;
        if (rule.rotation)
        {
            const glm::quat sourceRotation(source->transform.rotation);
            const glm::quat inherited = glm::slerp(
                glm::quat(1.f, 0.f, 0.f, 0.f), sourceRotation,
                rule.influence);
            transform.rotation = glm::eulerAngles(glm::normalize(
                glm::quat(transform.rotation) * inherited));
        }
    }
    for (auto& [node, last] : m_lastPose)
        if (Object* bone = model->ResolveNode(node))
        {
            last.outputPosition = bone->transform.position;
            last.outputRotation = bone->transform.rotation;
            last.applied = true;
        }
}

void RigConstraints::PostPhysicsUpdate()
{
    if (Model* model = Owner ? Owner->GetComponent<Model>() : nullptr)
        ApplyConstraints(*model, true);
}

bool RigConstraints::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    const std::string appendCount = std::to_string(appendRules.size());
    const std::string ikCount = std::to_string(ikChains.size());
    ui.ValueLabel("Append rules", appendCount.c_str());
    ui.ValueLabel("IK chains", ikCount.c_str());
    if (!ikChains.empty()) {
        if (ui.Button("Disable all IK"))
            for (auto& chain : ikChains) {
                changed = chain.enabled || changed;
                chain.enabled = false;
            }
        ui.SameLine();
        if (ui.Button("Enable all IK"))
            for (auto& chain : ikChains) {
                changed = !chain.enabled || changed;
                chain.enabled = true;
            }
        Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
        for (size_t i = 0; i < ikChains.size(); ++i) {
            const Object* controller = model ? model->ResolveNode(ikChains[i].controllerNodeIndex) : nullptr;
            const std::string label = controller ? controller->name : "IK chain " + std::to_string(i + 1);
            ui.PushId(std::to_string(i).c_str());
            changed = ui.Checkbox(label.c_str(), &ikChains[i].enabled) || changed;
            if (controller)
                if (const auto animated = m_animatedIkSwitches.find(controller->name);
                    animated != m_animatedIkSwitches.end())
                    ui.ValueLabel("Animation override", animated->second ? "On" : "Off");
            ui.PopId();
        }
    }
    if (ui.PropertyGroupHeader("External Parents"))
        for (auto& binding : externalParents) {
            char name[256]{};
            strncpy_s(name, sizeof(name), binding.targetName.c_str(), _TRUNCATE);
            if (ui.InputText(("Key " + std::to_string(binding.key) + " target object").c_str(), name, sizeof(name))) {
                binding.targetName = name;
                m_externalRests.erase(binding.nodeIndex);
                changed = true;
            }
        }
    if (changed) MarkConfigurationDirty();
    return changed;
}

RigConstraints::JsonValue RigConstraints::Serialize() const
{
    RigController legacy;
    legacy.appendRules = appendRules;
    legacy.ikChains = ikChains;
    legacy.fixedAxes = fixedAxes;
    legacy.localAxes = localAxes;
    legacy.externalParents = externalParents;
    JsonValue value = legacy.Serialize();
    value.Set("type", JsonValue("RigConstraints"));
    value.Erase("physicsBones");
    value.Erase("physicsJoints");
    value.Erase("boneMorphs");
    value.Erase("impulseMorphs");
    return value;
}

void RigConstraints::Deserialize(const JsonValue& value)
{
    RigController legacy;
    legacy.Deserialize(value);
    Component::Deserialize(value);
    appendRules = std::move(legacy.appendRules);
    ikChains = std::move(legacy.ikChains);
    fixedAxes = std::move(legacy.fixedAxes);
    localAxes = std::move(legacy.localAxes);
    externalParents = std::move(legacy.externalParents);
    Start();
}
}
