#include "RigController.h"
#include "RigConstraints.h"
#include "RigMorphControls.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"

#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
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

}

RigController::RigController()
{
    SetTypeName(COMPONENT_TYPE_NAME(RigController));
    singlecomponent = true;
    editorAddable = false;
}

void RigController::Start()
{
    m_animatedIkSwitches.clear();
    m_animatedMorphWeights.clear();
    m_externalRests.clear();
}

void RigController::EnsureComponents()
{
    if (!Owner) return;
    if (!appendRules.empty() || !ikChains.empty() || !fixedAxes.empty() ||
        !localAxes.empty() || !externalParents.empty())
        if (!Owner->GetComponent<RigConstraints>())
        {
            auto* constraints = Owner->AddComponent<RigConstraints>();
            constraints->appendRules = appendRules;
            constraints->ikChains = ikChains;
            constraints->fixedAxes = fixedAxes;
            constraints->localAxes = localAxes;
            constraints->externalParents = externalParents;
            constraints->SetAnimatedIkSwitches(m_animatedIkSwitches);
        }
    if (!boneMorphs.empty() || !impulseMorphs.empty())
        if (!Owner->GetComponent<RigMorphControls>())
        {
            auto* morphs = Owner->AddComponent<RigMorphControls>();
            morphs->boneMorphs = boneMorphs;
            morphs->impulseMorphs = impulseMorphs;
            morphs->SetAnimatedMorphWeights(m_animatedMorphWeights);
        }
    if (!physicsBones.empty() || !physicsJoints.empty())
        if (!Owner->GetComponent<RigPhysicsBinding>())
        {
            auto* binding = Owner->AddComponent<RigPhysicsBinding>();
            binding->physicsBones = physicsBones;
            binding->physicsJoints = physicsJoints;
        }
}

void RigController::SetAnimatedIkSwitches(
    const std::unordered_map<std::string, bool>& switches)
{
    m_animatedIkSwitches = switches;
    if (Owner)
        if (auto* constraints = Owner->GetComponent<RigConstraints>())
            constraints->SetAnimatedIkSwitches(switches);
}

void RigController::SetAnimatedMorphWeights(
    const std::unordered_map<std::string, float>& weights)
{
    m_animatedMorphWeights = weights;
    if (Owner)
        if (auto* morphs = Owner->GetComponent<RigMorphControls>())
            morphs->SetAnimatedMorphWeights(weights);
}

void RigController::Apply()
{
    EnsureComponents();
    if (!Owner) return;
    auto* constraints = Owner->GetComponent<RigConstraints>();
    auto* morphs = Owner->GetComponent<RigMorphControls>();
    if (constraints)
    {
        constraints->appendRules = appendRules;
        constraints->ikChains = ikChains;
        constraints->fixedAxes = fixedAxes;
        constraints->localAxes = localAxes;
        constraints->externalParents = externalParents;
    }
    if (morphs)
    {
        morphs->boneMorphs = boneMorphs;
        morphs->impulseMorphs = impulseMorphs;
    }
    if (auto* binding = Owner->GetComponent<RigPhysicsBinding>())
    {
        binding->physicsBones = physicsBones;
        binding->physicsJoints = physicsJoints;
    }
    if (constraints) constraints->RestorePose();
    if (morphs) morphs->ApplyBoneMorphs();
    if (constraints) constraints->Apply();
    if (auto* binding = Owner->GetComponent<RigPhysicsBinding>())
        binding->Apply();
    if (morphs) morphs->ApplyImpulseMorphs();
}

void RigController::PostPhysicsUpdate()
{
    EnsureComponents();
    if (!Owner) return;
    if (auto* binding = Owner->GetComponent<RigPhysicsBinding>())
    {
        binding->physicsBones = physicsBones;
        binding->physicsJoints = physicsJoints;
        binding->PostPhysicsUpdate();
    }
    if (auto* constraints = Owner->GetComponent<RigConstraints>())
    {
        constraints->RefreshPoseOutputs();
        constraints->PostPhysicsUpdate();
    }
}

bool RigController::DrawProperties(::Engine::Editor::IEditorUi& ui)
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
    ui.ValueLabel("Physics joints", jointCount.c_str());
    const std::string impulseCount = std::to_string(impulseMorphs.size());
    ui.ValueLabel("Impulse morphs", impulseCount.c_str());

    // A PMX morph can affect several meshes and materials as well as bones.
    // Keep each component's authored weight, but present one control by name.
    struct MorphControl
    {
        std::string name;
        float value = 0.f;
        std::vector<float*> weights;
        std::vector<Mesh*> meshes;
        std::vector<MaterialPropertyMorph*> materials;
    };
    std::vector<MorphControl> controls;
    std::unordered_map<std::string, size_t> controlIndices;
    const auto addWeight = [&](const std::string& name, float& weight)
        -> MorphControl&
    {
        auto [found, inserted] = controlIndices.try_emplace(name,
            controls.size());
        if (inserted)
        {
            auto& control = controls.emplace_back();
            control.name = name;
            control.value = weight;
        }
        auto& control = controls[found->second];
        control.weights.push_back(&weight);
        return control;
    };
    for (auto& morph : boneMorphs)
        addWeight(morph.name, morph.weight);
    for (auto& morph : impulseMorphs)
        addWeight(morph.name, morph.weight);
    const auto collectMorphs = [&](auto&& self, Object* object) -> void
    {
        if (!object) return;
        if (auto* material = object->GetComponent<MaterialPropertyMorph>())
            for (auto& morph : material->morphs)
                addWeight(morph.name, morph.weight).materials.push_back(material);
        if (auto* mesh = object->GetComponent<Mesh>())
        {
            const auto& targets = mesh->GetMorphTargets();
            auto& weights = mesh->GetMorphWeights();
            for (size_t i = 0; i < targets.size() && i < weights.size(); ++i)
                addWeight(targets[i].name, weights[i]).meshes.push_back(mesh);
        }
        for (auto* child : object->Children) self(self, child);
    };
    collectMorphs(collectMorphs, Owner);
    if ((!ikChains.empty() || !controls.empty()) &&
        ui.PropertyGroupHeader("Toggles"))
    {
        ui.Indent(12.f);
        if (!ikChains.empty() && ui.PropertyGroupHeader("IK"))
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
                const std::string button = label +
                    (ikChains[i].enabled ? " [On]" : " [Off]");
                if (ui.Button(button.c_str()))
                {
                    ikChains[i].enabled = !ikChains[i].enabled;
                    changed = true;
                }
                ui.PopId();
            }
            ui.Unindent(12.f);
        }
        if (!controls.empty() && ui.PropertyGroupHeader("Morphs"))
        {
            ui.Indent(12.f);
            for (size_t i = 0; i < controls.size(); ++i)
            {
                auto& control = controls[i];
                const std::string id = std::to_string(i);
                ui.PushId(id.c_str());
                if (ui.DragFloat(control.name.c_str(), &control.value,
                    .01f, 0.f, 1.f))
                {
                    for (float* weight : control.weights)
                        *weight = control.value;
                    for (Mesh* mesh : control.meshes)
                        mesh->SyncMorphWeights();
                    for (MaterialPropertyMorph* material : control.materials)
                        material->MarkConfigurationDirty();
                    changed = true;
                }
                ui.PopId();
            }
            ui.Unindent(12.f);
        }
        ui.Unindent(12.f);
    }
    if (!externalParents.empty() &&
        ui.PropertyGroupHeader("External Parents"))
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
    if (changed)
    {
        MarkConfigurationDirty();
        if (Owner)
        {
            if (auto* constraints = Owner->GetComponent<RigConstraints>())
            {
                constraints->appendRules = appendRules;
                constraints->ikChains = ikChains;
                constraints->fixedAxes = fixedAxes;
                constraints->localAxes = localAxes;
                constraints->externalParents = externalParents;
                constraints->MarkConfigurationDirty();
            }
            if (auto* morphs = Owner->GetComponent<RigMorphControls>())
            {
                morphs->boneMorphs = boneMorphs;
                morphs->impulseMorphs = impulseMorphs;
                morphs->MarkConfigurationDirty();
            }
        }
    }
    return changed;
}

RigController::JsonValue RigController::Serialize() const
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
            .Set("mode", Value(static_cast<int>(binding.mode)))
            .Set("offsetPosition", Vec3Value(binding.offsetPosition))
            .Set("offsetRotation", QuaternionValue(binding.offsetRotation)));
    result.Set("physicsBones", std::move(bodies));
    Value joints = Value::MakeArray();
    for (const auto& joint : physicsJoints)
        joints.Push(Value::MakeObject()
            .Set("name", Value(joint.name))
            .Set("type", Value(static_cast<int>(joint.type)))
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
    result.Set("boneMorphs", std::move(morphs));
    Value impulses = Value::MakeArray();
    for (const auto& morph : impulseMorphs)
    {
        Value offsets = Value::MakeArray();
        for (const auto& offset : morph.offsets)
            offsets.Push(Value::MakeObject()
                .Set("body", Value(offset.bodyIndex))
                .Set("bodyName", Value(offset.bodyName))
                .Set("local", Value(offset.local))
                .Set("velocity", Vec3Value(offset.velocity))
                .Set("torque", Vec3Value(offset.torque)));
        impulses.Push(Value::MakeObject()
            .Set("name", Value(morph.name))
            .Set("weight", Value(morph.weight))
            .Set("offsets", std::move(offsets)));
    }
    return result.Set("impulseMorphs", std::move(impulses));
}

void RigController::Deserialize(const JsonValue& value)
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
    impulseMorphs.clear();
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
        binding.mode = static_cast<Engine::Model::RigidBodyMotion>(
            source["mode"].AsInt());
        binding.offsetPosition = ReadVec3(source["offsetPosition"]);
        binding.offsetRotation = ReadQuaternion(source["offsetRotation"]);
    }
    for (size_t i = 0; i < value["physicsJoints"].ArraySize(); ++i)
    {
        const auto& source = value["physicsJoints"].ArrayAt(i);
        auto& joint = physicsJoints.emplace_back();
        joint.name = source["name"].AsString();
        joint.type = static_cast<Engine::Model::PhysicsJointKind>(
            source.Has("type") ? source["type"].AsInt() : 0);
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
    for (size_t i = 0; i < value["impulseMorphs"].ArraySize(); ++i)
    {
        const auto& source = value["impulseMorphs"].ArrayAt(i);
        auto& morph = impulseMorphs.emplace_back();
        morph.name = source["name"].AsString();
        morph.weight = source["weight"].AsFloat();
        for (size_t j = 0; j < source["offsets"].ArraySize(); ++j)
        {
            const auto& item = source["offsets"].ArrayAt(j);
            auto& offset = morph.offsets.emplace_back();
            offset.bodyIndex = item["body"].AsInt();
            offset.bodyName = item["bodyName"].AsString();
            offset.local = item["local"].AsBool();
            offset.velocity = ReadVec3(item["velocity"]);
            offset.torque = ReadVec3(item["torque"]);
        }
    }
    Start();
}
}
