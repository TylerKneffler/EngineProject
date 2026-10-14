#include "PmxRigController.h"

#include "Core/Compoonents/Animation/Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
    Object* controller = model.ResolveNode(chain.controllerNodeIndex);
    Object* effector = model.ResolveNode(chain.effectorNodeIndex);
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
            Object* joint = model.ResolveNode(link.nodeIndex);
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

PmxRigController::PmxRigController()
{
    SetTypeName(COMPONENT_TYPE_NAME(PmxRigController));
    editorAddable = false;
    singlecomponent = true;
}

void PmxRigController::Start()
{
    m_lastPose.clear();
    m_animatedIkSwitches.clear();
    m_animatedMorphWeights.clear();
    m_externalRests.clear();
    m_physicsInitialized = false;
}

void PmxRigController::SetAnimatedIkSwitches(
    const std::unordered_map<std::string, bool>& switches)
{
    m_animatedIkSwitches = switches;
}

void PmxRigController::SetAnimatedMorphWeights(
    const std::unordered_map<std::string, float>& weights)
{
    m_animatedMorphWeights = weights;
}

void PmxRigController::Apply()
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
    std::unordered_map<unsigned, LastAppend> nextPose;
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
    m_lastPose = std::move(nextPose);
    ApplyConstraints(*model, false);
    if (Owner && !physicsBones.empty())
    {
        for (const auto& binding : physicsBones)
        {
            Object* bone = model->ResolveNode(binding.nodeIndex);
            Object* bodyObject = Owner->FindObjectInChildrenByName(
                binding.bodyName);
            auto* body = bodyObject ? bodyObject->GetComponent<RigidBody>() : nullptr;
            if (!bone || !body) continue;
            const glm::mat4 offset = glm::translate(glm::mat4(1.f),
                binding.offsetPosition) * glm::mat4_cast(binding.offsetRotation);
            const glm::mat4 world = bone->transform.GetWorldMatrix() * offset;
            if (!m_physicsInitialized)
                body->SetWorldPose(glm::vec3(world[3]), MatrixRotation(world));
            else if (binding.mode == 2)
                body->SetWorldPosition(glm::vec3(world[3]));
        }
        m_physicsInitialized = true;
    }
}

void PmxRigController::ApplyConstraints(Model& modelValue, bool afterPhysics)
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
    struct DeformStep
    {
        int layer;
        int order;
        int kind; // Append, IK, external parent.
        size_t index;
    };
    std::vector<DeformStep> steps;
    steps.reserve(appendRules.size() + ikChains.size() +
        externalParents.size());
    for (size_t index = 0; index < appendRules.size(); ++index)
    {
        const auto& rule = appendRules[index];
        steps.push_back({ rule.deformLayer,
            rule.deformOrder < 0 ? static_cast<int>(rule.nodeIndex) :
                rule.deformOrder, 0, index });
    }
    for (size_t index = 0; index < ikChains.size(); ++index)
    {
        const auto& chain = ikChains[index];
        steps.push_back({ chain.deformLayer,
            chain.deformOrder < 0 ?
                static_cast<int>(chain.controllerNodeIndex) :
                chain.deformOrder, 1, index });
    }
    for (size_t index = 0; index < externalParents.size(); ++index)
    {
        const auto& binding = externalParents[index];
        steps.push_back({ binding.deformLayer,
            binding.deformOrder < 0 ? static_cast<int>(binding.nodeIndex) :
                binding.deformOrder, 2, index });
    }
    std::stable_sort(steps.begin(), steps.end(),
        [](const DeformStep& a, const DeformStep& b)
        {
            if (a.layer != b.layer) return a.layer < b.layer;
            if (a.order != b.order) return a.order < b.order;
            return a.kind < b.kind;
        });
    for (const auto& step : steps)
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

void PmxRigController::PostPhysicsUpdate()
{
    Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
    if (!model) return;
    for (const auto& binding : physicsBones)
    {
        Object* bone = model->ResolveNode(binding.nodeIndex);
        Object* bodyObject = Owner->FindObjectInChildrenByName(
            binding.bodyName);
        if (!bone || !bodyObject) continue;
        const glm::mat4 offset = glm::translate(glm::mat4(1.f),
            binding.offsetPosition) * glm::mat4_cast(binding.offsetRotation);
        const glm::mat4 bodyWorld = bodyObject->transform.GetWorldMatrix();
        const glm::mat4 desiredWorld = bodyWorld * glm::inverse(offset);
        const glm::mat4 parentWorld = bone->Parent
            ? bone->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
        if (std::abs(glm::determinant(parentWorld)) < 1e-8f) continue;
        if (binding.mode == 1)
            bone->transform.position = glm::vec3(glm::inverse(parentWorld) *
                glm::vec4(glm::vec3(desiredWorld[3]), 1.f));
        const glm::quat parentRotation = bone->Parent
            ? WorldRotation(*bone->Parent) : glm::quat(1.f, 0.f, 0.f, 0.f);
        bone->transform.rotation = glm::eulerAngles(glm::normalize(
            glm::inverse(parentRotation) * MatrixRotation(desiredWorld)));
        if (auto found = m_lastPose.find(binding.nodeIndex);
            found != m_lastPose.end())
        {
            found->second.outputPosition = bone->transform.position;
            found->second.outputRotation = bone->transform.rotation;
        }
    }
    ApplyConstraints(*model, true);
}

bool PmxRigController::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    const std::string appendCount = std::to_string(appendRules.size());
    const std::string ikCount = std::to_string(ikChains.size());
    ui.ValueLabel("Append rules", appendCount.c_str());
    ui.ValueLabel("IK chains", ikCount.c_str());
    const std::string localAxisCount = std::to_string(localAxes.size());
    ui.ValueLabel("Local-axis bones", localAxisCount.c_str());
    const std::string physicsCount = std::to_string(physicsBones.size());
    ui.ValueLabel("Physics-driven bones", physicsCount.c_str());
    const std::string jointCount = std::to_string(physicsJoints.size());
    ui.ValueLabel("PMX physics joints", jointCount.c_str());
    if (!ikChains.empty() && ui.PropertyGroupHeader("PMX IK"))
    {
        ui.Indent(12.f);
        Model* model = Owner ? Owner->GetComponent<Model>() : nullptr;
        for (size_t i = 0; i < ikChains.size(); ++i)
        {
            const std::string id = std::to_string(i);
            ui.PushId(id.c_str());
            const Object* controller = model
                ? model->ResolveNode(ikChains[i].controllerNodeIndex) : nullptr;
            const std::string label = controller
                ? controller->name : "IK chain " + std::to_string(i + 1);
            changed |= ui.Checkbox(label.c_str(), &ikChains[i].enabled);
            ui.PopId();
        }
        ui.Unindent(12.f);
    }
    if (!boneMorphs.empty() && ui.PropertyGroupHeader("Bone Morphs"))
    {
        ui.Indent(12.f);
        for (size_t i = 0; i < boneMorphs.size(); ++i)
        {
            const std::string id = std::to_string(i);
            ui.PushId(id.c_str());
            changed |= ui.DragFloat(boneMorphs[i].name.c_str(),
                &boneMorphs[i].weight, .01f, 0.f, 1.f);
            ui.PopId();
        }
        ui.Unindent(12.f);
    }
    if (!externalParents.empty() &&
        ui.PropertyGroupHeader("PMX External Parents"))
    {
        ui.Indent(12.f);
        for (size_t i = 0; i < externalParents.size(); ++i)
        {
            auto& binding = externalParents[i];
            const std::string id = std::to_string(i);
            ui.PushId(id.c_str());
            char targetName[256]{};
            strncpy_s(targetName, sizeof(targetName),
                binding.targetName.c_str(), _TRUNCATE);
            const std::string label = "Key " + std::to_string(binding.key) +
                " target object";
            if (ui.InputText(label.c_str(), targetName, sizeof(targetName)))
            {
                binding.targetName = targetName;
                m_externalRests.erase(binding.nodeIndex);
                changed = true;
            }
            ui.PopId();
        }
        ui.Unindent(12.f);
    }
    if (changed) MarkConfigurationDirty();
    return changed;
}

PmxRigController::JsonValue PmxRigController::Serialize() const
{
    Value result = Component::Serialize();
    Value appends = Value::MakeArray();
    for (const auto& rule : appendRules)
        appends.Push(Value::MakeObject()
            .Set("node", Value(static_cast<int>(rule.nodeIndex)))
            .Set("source", Value(static_cast<int>(rule.sourceNodeIndex)))
            .Set("sourceRest", Vec3Value(rule.sourceRestTranslation))
            .Set("influence", Value(rule.influence))
            .Set("layer", Value(rule.deformLayer))
            .Set("order", Value(rule.deformOrder))
            .Set("afterPhysics", Value(rule.afterPhysics))
            .Set("rotation", Value(rule.rotation))
            .Set("translation", Value(rule.translation)));
    result.Set("appends", std::move(appends));
    Value chains = Value::MakeArray();
    for (const auto& chain : ikChains)
    {
        Value links = Value::MakeArray();
        for (const auto& link : chain.links)
            links.Push(Value::MakeObject()
                .Set("node", Value(static_cast<int>(link.nodeIndex)))
                .Set("limited", Value(link.limited))
                .Set("minimum", Vec3Value(link.minimum))
                .Set("maximum", Vec3Value(link.maximum))
                .Set("fixedAxis", Value(link.fixedAxis))
                .Set("axis", Vec3Value(link.axis)));
        chains.Push(Value::MakeObject()
            .Set("controller", Value(static_cast<int>(chain.controllerNodeIndex)))
            .Set("effector", Value(static_cast<int>(chain.effectorNodeIndex)))
            .Set("enabled", Value(chain.enabled))
            .Set("iterations", Value(chain.iterations))
            .Set("angleLimit", Value(chain.angleLimit))
            .Set("layer", Value(chain.deformLayer))
            .Set("order", Value(chain.deformOrder))
            .Set("afterPhysics", Value(chain.afterPhysics))
            .Set("links", std::move(links)));
    }
    result.Set("ikChains", std::move(chains));
    Value axes = Value::MakeArray();
    for (const auto& fixed : fixedAxes)
        axes.Push(Value::MakeObject()
            .Set("node", Value(static_cast<int>(fixed.nodeIndex)))
            .Set("axis", Vec3Value(fixed.axis)));
    result.Set("fixedAxes", std::move(axes));
    Value localAxisValues = Value::MakeArray();
    for (const auto& local : localAxes)
        localAxisValues.Push(Value::MakeObject()
            .Set("node", Value(static_cast<int>(local.nodeIndex)))
            .Set("x", Vec3Value(local.x))
            .Set("z", Vec3Value(local.z))
            .Set("restTranslation", Vec3Value(local.restTranslation)));
    result.Set("localAxes", std::move(localAxisValues));
    Value parents = Value::MakeArray();
    for (const auto& binding : externalParents)
        parents.Push(Value::MakeObject()
            .Set("node", Value(static_cast<int>(binding.nodeIndex)))
            .Set("key", Value(binding.key))
            .Set("layer", Value(binding.deformLayer))
            .Set("order", Value(binding.deformOrder))
            .Set("afterPhysics", Value(binding.afterPhysics))
            .Set("targetName", Value(binding.targetName)));
    result.Set("externalParents", std::move(parents));
    Value bodies = Value::MakeArray();
    for (const auto& binding : physicsBones)
        bodies.Push(Value::MakeObject()
            .Set("bodyName", Value(binding.bodyName))
            .Set("node", Value(static_cast<int>(binding.nodeIndex)))
            .Set("mode", Value(binding.mode))
            .Set("offsetPosition", Vec3Value(binding.offsetPosition))
            .Set("offsetRotation", QuaternionValue(binding.offsetRotation)));
    result.Set("physicsBones", std::move(bodies));
    Value joints = Value::MakeArray();
    for (const auto& joint : physicsJoints)
        joints.Push(Value::MakeObject()
            .Set("name", Value(joint.name))
            .Set("type", Value(joint.type))
            .Set("bodyA", Value(joint.bodyA))
            .Set("bodyB", Value(joint.bodyB))
            .Set("bodyAName", Value(joint.bodyAName))
            .Set("bodyBName", Value(joint.bodyBName))
            .Set("position", Vec3Value(joint.position))
            .Set("rotation", QuaternionValue(joint.rotation))
            .Set("linearLower", Vec3Value(joint.linearLower))
            .Set("linearUpper", Vec3Value(joint.linearUpper))
            .Set("angularLower", Vec3Value(joint.angularLower))
            .Set("angularUpper", Vec3Value(joint.angularUpper))
            .Set("linearSpring", Vec3Value(joint.linearSpring))
            .Set("angularSpring", Vec3Value(joint.angularSpring))
            .Set("frameAPosition", Vec3Value(joint.frameAPosition))
            .Set("frameARotation", QuaternionValue(joint.frameARotation))
            .Set("frameBPosition", Vec3Value(joint.frameBPosition))
            .Set("frameBRotation", QuaternionValue(joint.frameBRotation)));
    result.Set("physicsJoints", std::move(joints));
    Value morphs = Value::MakeArray();
    for (const auto& morph : boneMorphs)
    {
        Value offsets = Value::MakeArray();
        for (const auto& offset : morph.offsets)
            offsets.Push(Value::MakeObject()
                .Set("node", Value(static_cast<int>(offset.nodeIndex)))
                .Set("translation", Vec3Value(offset.translation))
                .Set("rotation", QuaternionValue(offset.rotation)));
        morphs.Push(Value::MakeObject()
            .Set("name", Value(morph.name))
            .Set("weight", Value(morph.weight))
            .Set("offsets", std::move(offsets)));
    }
    return result.Set("boneMorphs", std::move(morphs));
}

void PmxRigController::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    appendRules.clear();
    ikChains.clear();
    fixedAxes.clear();
    localAxes.clear();
    externalParents.clear();
    physicsBones.clear();
    physicsJoints.clear();
    boneMorphs.clear();
    for (size_t i = 0; i < value["appends"].ArraySize(); ++i)
    {
        const auto& item = value["appends"].ArrayAt(i);
        auto& rule = appendRules.emplace_back();
        rule.nodeIndex = static_cast<unsigned>(item["node"].AsInt());
        rule.sourceNodeIndex = static_cast<unsigned>(item["source"].AsInt());
        rule.sourceRestTranslation = ReadVec3(item["sourceRest"]);
        rule.influence = item["influence"].AsFloat();
        rule.deformLayer = item["layer"].AsInt();
        rule.deformOrder = item.Has("order") ? item["order"].AsInt() : -1;
        rule.afterPhysics = item["afterPhysics"].AsBool();
        rule.rotation = item["rotation"].AsBool();
        rule.translation = item["translation"].AsBool();
    }
    for (size_t i = 0; i < value["ikChains"].ArraySize(); ++i)
    {
        const auto& item = value["ikChains"].ArrayAt(i);
        auto& chain = ikChains.emplace_back();
        chain.controllerNodeIndex = static_cast<unsigned>(item["controller"].AsInt());
        chain.effectorNodeIndex = static_cast<unsigned>(item["effector"].AsInt());
        chain.enabled = !item.Has("enabled") || item["enabled"].AsBool();
        chain.iterations = item["iterations"].AsInt();
        chain.angleLimit = item["angleLimit"].AsFloat();
        chain.deformLayer = item["layer"].AsInt();
        chain.deformOrder = item.Has("order") ? item["order"].AsInt() : -1;
        chain.afterPhysics = item["afterPhysics"].AsBool();
        for (size_t j = 0; j < item["links"].ArraySize(); ++j)
        {
            const auto& source = item["links"].ArrayAt(j);
            auto& link = chain.links.emplace_back();
            link.nodeIndex = static_cast<unsigned>(source["node"].AsInt());
            link.limited = source["limited"].AsBool();
            link.minimum = ReadVec3(source["minimum"]);
            link.maximum = ReadVec3(source["maximum"]);
            link.fixedAxis = source["fixedAxis"].AsBool();
            link.axis = ReadVec3(source["axis"]);
        }
    }
    for (size_t i = 0; i < value["fixedAxes"].ArraySize(); ++i)
    {
        const auto& source = value["fixedAxes"].ArrayAt(i);
        fixedAxes.push_back({ static_cast<unsigned>(source["node"].AsInt()),
            ReadVec3(source["axis"]) });
    }
    for (size_t i = 0; i < value["localAxes"].ArraySize(); ++i)
    {
        const auto& source = value["localAxes"].ArrayAt(i);
        localAxes.push_back({ static_cast<unsigned>(source["node"].AsInt()),
            ReadVec3(source["x"]), ReadVec3(source["z"]),
            ReadVec3(source["restTranslation"]) });
    }
    for (size_t i = 0; i < value["externalParents"].ArraySize(); ++i)
    {
        const auto& source = value["externalParents"].ArrayAt(i);
        auto& binding = externalParents.emplace_back();
        binding.nodeIndex = static_cast<unsigned>(source["node"].AsInt());
        binding.key = source["key"].AsInt();
        binding.deformLayer = source["layer"].AsInt();
        binding.deformOrder = source.Has("order") ?
            source["order"].AsInt() : -1;
        binding.afterPhysics = source["afterPhysics"].AsBool();
        binding.targetName = source["targetName"].AsString();
    }
    for (size_t i = 0; i < value["physicsBones"].ArraySize(); ++i)
    {
        const auto& source = value["physicsBones"].ArrayAt(i);
        auto& binding = physicsBones.emplace_back();
        binding.bodyName = source["bodyName"].AsString();
        binding.nodeIndex = static_cast<unsigned>(source["node"].AsInt());
        binding.mode = source["mode"].AsInt();
        binding.offsetPosition = ReadVec3(source["offsetPosition"]);
        binding.offsetRotation = ReadQuaternion(source["offsetRotation"]);
    }
    for (size_t i = 0; i < value["physicsJoints"].ArraySize(); ++i)
    {
        const auto& source = value["physicsJoints"].ArrayAt(i);
        auto& joint = physicsJoints.emplace_back();
        joint.name = source["name"].AsString();
        joint.type = source.Has("type") ? source["type"].AsInt() : 0;
        joint.bodyA = source["bodyA"].AsInt();
        joint.bodyB = source["bodyB"].AsInt();
        joint.bodyAName = source["bodyAName"].AsString();
        joint.bodyBName = source["bodyBName"].AsString();
        joint.position = ReadVec3(source["position"]);
        joint.rotation = ReadQuaternion(source["rotation"]);
        joint.linearLower = ReadVec3(source["linearLower"]);
        joint.linearUpper = ReadVec3(source["linearUpper"]);
        joint.angularLower = ReadVec3(source["angularLower"]);
        joint.angularUpper = ReadVec3(source["angularUpper"]);
        joint.linearSpring = ReadVec3(source["linearSpring"]);
        joint.angularSpring = ReadVec3(source["angularSpring"]);
        joint.frameAPosition = ReadVec3(source["frameAPosition"]);
        joint.frameARotation = ReadQuaternion(source["frameARotation"]);
        joint.frameBPosition = ReadVec3(source["frameBPosition"]);
        joint.frameBRotation = ReadQuaternion(source["frameBRotation"]);
    }
    for (size_t i = 0; i < value["boneMorphs"].ArraySize(); ++i)
    {
        const auto& item = value["boneMorphs"].ArrayAt(i);
        auto& morph = boneMorphs.emplace_back();
        morph.name = item["name"].AsString();
        morph.weight = item["weight"].AsFloat();
        for (size_t j = 0; j < item["offsets"].ArraySize(); ++j)
        {
            const auto& source = item["offsets"].ArrayAt(j);
            auto& offset = morph.offsets.emplace_back();
            offset.nodeIndex = static_cast<unsigned>(source["node"].AsInt());
            offset.translation = ReadVec3(source["translation"]);
            offset.rotation = ReadQuaternion(source["rotation"]);
        }
    }
    Start();
}
}
