#include "Skeleton.h"
#include "AnimationBone.h"
#include "IKBone.h"
#include "Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace Engine::Components
{
namespace
{
std::string LowerJointType(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(
            std::tolower(character)); });
    return value;
}

void ClampJointAxis(float& angle, float& velocity, float limit)
{
    const float bounded = std::clamp(angle, -std::max(limit, 0.f),
        std::max(limit, 0.f));
    if (angle != bounded && velocity * angle > 0.f)
        velocity = 0.f;
    angle = bounded;
}
}

Skeleton::Skeleton()
{
    SetTypeName(COMPONENT_TYPE_NAME(Skeleton));
    RegisterField("modelReference", modelReference);
    RegisterField("colliderMode", colliderMode, "Collision");
    RegisterField("meshColliderReference", meshColliderReference, "Collision");
    RegisterField("meshContactResponseEnabled", meshContactResponseEnabled, "Collision | Bone Response");
    RegisterField("meshContactResponseStrength", meshContactResponseStrength, "Collision | Bone Response");
    RegisterField("meshContactDamping", meshContactDamping, "Collision | Bone Response");
    RegisterField("meshContactMaxBend", meshContactMaxBend, "Collision | Bone Response");
    RegisterField("showBones", showBones);
}

Skeleton::JsonValue Skeleton::Serialize() const
{
    JsonValue result = Component::Serialize()
        .Set("skinIndex", JsonValue(static_cast<int>(skinIndex)));
    JsonValue nodes = JsonValue::MakeArray(), matrices = JsonValue::MakeArray();
    for (unsigned node : jointNodes)
        nodes.Push(JsonValue(static_cast<int>(node)));
    for (const glm::mat4& matrix : inverseBindMatrices)
    {
        JsonValue serialized = JsonValue::MakeArray();
        const float* data = &matrix[0][0];
        for (size_t i = 0; i < 16; ++i) serialized.Push(JsonValue(data[i]));
        matrices.Push(std::move(serialized));
    }
    return result.Set("joints", std::move(nodes))
        .Set("inverseBindMatrices", std::move(matrices));
}

void Skeleton::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    skinIndex = static_cast<unsigned>(value["skinIndex"].AsInt());
    jointNodes.clear();
    inverseBindMatrices.clear();
    for (size_t i = 0; i < value["joints"].ArraySize(); ++i)
        jointNodes.push_back(static_cast<unsigned>(value["joints"].ArrayAt(i).AsInt()));
    for (size_t i = 0; i < value["inverseBindMatrices"].ArraySize(); ++i)
    {
        glm::mat4 matrix(1.f);
        float* data = &matrix[0][0];
        for (size_t j = 0; j < 16; ++j)
            data[j] = value["inverseBindMatrices"].ArrayAt(i).ArrayAt(j).AsFloat();
        inverseBindMatrices.push_back(matrix);
    }
    m_modelCacheValid = false;
    m_cachedJoints.clear();
    ResetMeshContactResponse();
}

bool Skeleton::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    changed = DrawReferenceProperty(ui, "modelReference", "Model",
        modelReference, ResolveModel()) || changed;
    const bool meshColliderMode = UsesMeshCollider();
    if (ui.Button(meshColliderMode ? "Collision: Mesh Collider" : "Collision: Per Bone"))
    {
        colliderMode = meshColliderMode ? "PerBone" : "MeshCollider";
        changed = true;
    }
    if (UsesMeshCollider())
    {
        changed = DrawReferenceProperty(ui, "meshColliderReference",
            "Mesh Collider", meshColliderReference) || changed;
        MeshObjectCollider* collider = ResolveMeshCollider();
        if (!collider)
            ui.DisabledLabel("Assign a MeshObjectCollider to enable mesh collision.");
        else if (!collider->Owner ||
            !collider->Owner->GetComponent<RigidBody>())
            ui.DisabledLabel("Add a Kinematic RigidBody to the collider object.");
        else if (collider->Owner->GetComponent<RigidBody>()->bodyType == "Dynamic")
            ui.DisabledLabel("Dynamic mesh collision uses a convex hull of the current pose.");
        else
            ui.DisabledLabel("Mesh collider uses the current skin and morph pose.");
        changed = ui.Checkbox("Bone Contact Response",
            &meshContactResponseEnabled) || changed;
        if (meshContactResponseEnabled)
        {
            changed = ui.DragFloat("Contact Strength",
                &meshContactResponseStrength, 0.1f, 0.f, 100.f) || changed;
            changed = ui.DragFloat("Contact Damping",
                &meshContactDamping, 0.1f, 0.f, 100.f) || changed;
            changed = ui.DragFloat("Maximum Bend (Radians)",
                &meshContactMaxBend, 0.01f, 0.f, 1.57f) || changed;
        }
    }
    const char* label = showBones ? "Hide Bones in Scene" : "Show Bones in Scene";
    if (ui.Button(label))
    {
        showBones = !showBones;
        changed = true;
    }
    const std::string skin = std::to_string(skinIndex);
    const std::string bones = std::to_string(jointNodes.size());
    ui.ValueLabel("Skin", skin.c_str());
    ui.ValueLabel("Bones", bones.c_str());
    if (changed) MarkConfigurationDirty();
    return changed;
}

bool Skeleton::UsesMeshCollider() const
{
    return colliderMode == "MeshCollider";
}

void Skeleton::ResetMeshContactResponse()
{
    m_contactBoneStates.clear();
}

void Skeleton::ApplyMeshContactResponse(float stepSeconds)
{
    MeshObjectCollider* collider = UsesMeshCollider()
        ? ResolveMeshCollider() : nullptr;
    RigidBody* rootBody = collider && collider->Owner
        ? collider->Owner->GetComponent<RigidBody>() : nullptr;
    if (!meshContactResponseEnabled || !collider || !collider->collisionEnabled ||
        !rootBody)
    {
        ResetMeshContactResponse();
        return;
    }
    if (!std::isfinite(stepSeconds) || stepSeconds <= 0.f) return;
    const float dt = std::min(stepSeconds, 0.1f);
    const auto& bones = ResolveBones();
    for (auto it = m_contactBoneStates.begin();
        it != m_contactBoneStates.end();)
        if (std::find(bones.begin(), bones.end(), it->first) == bones.end())
            it = m_contactBoneStates.erase(it);
        else
            ++it;
    for (AnimationBone* bone : bones)
        if (bone && bone->Owner)
        {
            auto [it, inserted] = m_contactBoneStates.try_emplace(bone);
            const glm::quat current(bone->Owner->transform.rotation);
            // Animation or IK may have written a new pose since the last
            // physics step. Treat it as the new base while retaining the
            // accumulated contact bend.
            if (inserted || !it->second.hasAppliedPose ||
                std::abs(glm::dot(current,
                    it->second.lastAppliedRotation)) < 0.99999f)
                it->second.baseRotation = current;
        }

    for (const MeshObjectCollider::Contact& contact : collider->GetContacts())
    {
        if (!contact.surfaceMapped || contact.normalImpulse <= 0.f) continue;
        const glm::vec3 force = contact.normalWorld *
            std::min(contact.normalImpulse, 5.f);
        for (uint8_t index = 0; index < contact.boneWeightCount; ++index)
        {
            const MeshObjectCollider::BoneWeight& influence =
                contact.boneWeights[index];
            AnimationBone* bone = influence.bone;
            if (!bone || !bone->Owner || influence.weight <= 0.f ||
                std::find(bones.begin(), bones.end(), bone) == bones.end())
                continue;
            const glm::vec3 arm = contact.surfacePointWorld -
                bone->Owner->transform.GetWorldPosition();
            glm::vec3 torque = glm::cross(arm, force);
            if (bone->Owner->Parent)
                torque = glm::mat3(glm::inverse(
                    bone->Owner->Parent->transform.GetWorldMatrix())) * torque;
            if (!std::isfinite(torque.x) || !std::isfinite(torque.y) ||
                !std::isfinite(torque.z))
                continue;
            m_contactBoneStates[bone].angularVelocity +=
                torque * influence.weight *
                std::max(meshContactResponseStrength, 0.f);
        }
    }

    for (auto& [bone, state] : m_contactBoneStates)
    {
        if (!bone || !bone->Owner) continue;
        const float speed = glm::length(state.angularVelocity);
        if (speed > 8.f)
            state.angularVelocity *= 8.f / speed;
        float spring = 0.f;
        float damping = std::max(meshContactDamping, 0.f);
        IKBone* joint = bone->Owner->GetComponent<IKBone>();
        const bool hasParentJoint = joint && joint->connectToParent &&
            bone->GetParentBone();
        const std::string jointType = hasParentJoint
            ? LowerJointType(joint->jointType) : std::string();
        if (jointType == "spring" && !joint->IsSimulating())
        {
            spring = std::max(joint->springStiffness, 0.f);
            damping = std::max(damping, 2.f * std::sqrt(spring) *
                std::clamp(joint->springDamping, 0.f, 1.f));
        }
        state.angularVelocity -= spring * state.offset * dt;
        state.angularVelocity *= std::exp(-damping * dt);
        state.offset += state.angularVelocity * dt;
        if (jointType == "fixed")
        {
            state.offset = glm::vec3(0.f);
            state.angularVelocity = glm::vec3(0.f);
        }
        else if (jointType == "hinge")
        {
            ClampJointAxis(state.offset.x, state.angularVelocity.x, 0.f);
            ClampJointAxis(state.offset.y, state.angularVelocity.y, 0.f);
            ClampJointAxis(state.offset.z, state.angularVelocity.z,
                joint->hingeLimit);
        }
        else if (hasParentJoint)
        {
            ClampJointAxis(state.offset.x, state.angularVelocity.x,
                joint->twistLimit);
            ClampJointAxis(state.offset.y, state.angularVelocity.y,
                joint->swingLimit);
            ClampJointAxis(state.offset.z, state.angularVelocity.z,
                joint->swingLimit);
        }
        const float limit = std::max(meshContactMaxBend, 0.f);
        const float bend = glm::length(state.offset);
        if (bend > limit && bend > 0.f)
        {
            state.offset *= limit / bend;
            state.angularVelocity *= 0.5f;
        }
        const float appliedBend = glm::length(state.offset);
        const glm::quat bendRotation = appliedBend > 1e-6f
            ? glm::angleAxis(appliedBend, state.offset / appliedBend)
            : glm::quat(1.f, 0.f, 0.f, 0.f);
        state.lastAppliedRotation = glm::normalize(
            bendRotation * state.baseRotation);
        state.hasAppliedPose = true;
        bone->Owner->transform.rotation = glm::eulerAngles(
            state.lastAppliedRotation);
    }
}

MeshObjectCollider* Skeleton::ResolveMeshCollider() const
{
    if (!Owner) return nullptr;
    return meshColliderReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<MeshObjectCollider>(
            Owner, meshColliderReference)
        : Owner->GetComponent<MeshObjectCollider>();
}

Skeleton::Object* Skeleton::FindNode(unsigned index) const
{
    Model* model = ResolveModel();
    return model ? model->ResolveNode(index) : nullptr;
}

Skeleton::Object* Skeleton::GetHierarchyRoot() const
{
    const std::vector<AnimationBone*>& roots = ResolveRootBones();
    if (roots.size() == 1 && roots.front() && roots.front()->Owner)
        return roots.front()->Owner;
    Model* model = ResolveModel();
    return model ? model->Owner : Owner;
}

Model* Skeleton::ResolveModel() const
{
    const uint64_t structureRevision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    const uint64_t configurationRevision = GetConfigurationRevision();
    if (m_modelCacheValid &&
        m_cachedStructureRevision == structureRevision &&
        m_cachedConfigurationRevision == configurationRevision)
        return m_cachedModel;

    Model* model = modelReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Model>(Owner, modelReference) : nullptr;
    if (!modelReference.IsAssigned())
        for (Object* current = Owner; current && !model; current = current->Parent)
            model = current->GetComponent<Model>();
    m_cachedModel = model;
    m_cachedStructureRevision = structureRevision;
    m_cachedConfigurationRevision = configurationRevision;
    m_modelCacheValid = true;
    return m_cachedModel;
}

uint64_t Skeleton::JointBindingSignature() const
{
    uint64_t signature = 1469598103934665603ull;
    for (const unsigned node : jointNodes)
    {
        signature ^= node;
        signature *= 1099511628211ull;
    }
    signature ^= static_cast<uint64_t>(jointNodes.size());
    return signature;
}

const std::vector<Skeleton::Object*>& Skeleton::ResolveJoints() const
{
    Model* model = ResolveModel();
    const uint64_t modelRevision = model ? model->GetConfigurationRevision() : 0;
    const uint64_t jointSignature = JointBindingSignature();
    const uint64_t structureRevision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    if (m_cachedJointModel == model &&
        m_cachedModelRevision == modelRevision &&
        m_cachedJointSignature == jointSignature &&
        m_cachedJointStructureRevision == structureRevision &&
        m_cachedJoints.size() == jointNodes.size())
        return m_cachedJoints;

    m_cachedJoints.assign(jointNodes.size(), nullptr);
    if (!model)
    {
        m_cachedModelRevision = modelRevision;
        m_cachedJointSignature = jointSignature;
        m_cachedJointStructureRevision = structureRevision;
        m_cachedJointModel = model;
        return m_cachedJoints;
    }
    const std::vector<Object*>& nodes = model->ResolveNodes();
    size_t jointIndex = 0;
    for (const unsigned node : jointNodes)
        m_cachedJoints[jointIndex++] = node < nodes.size() ? nodes[node] : nullptr;
    m_cachedModelRevision = modelRevision;
    m_cachedJointSignature = jointSignature;
    m_cachedJointStructureRevision = structureRevision;
    m_cachedJointModel = model;
    return m_cachedJoints;
}

const std::vector<AnimationBone*>& Skeleton::ResolveBones() const
{
    const std::vector<Object*>& joints = ResolveJoints();
    m_cachedBones.assign(joints.size(), nullptr);
    for (size_t palette = 0; palette < joints.size(); ++palette)
        if (Object* joint = joints[palette])
            for (Engine::Core::Component* component : joint->Components)
                if (auto* bone = dynamic_cast<AnimationBone*>(component);
                    bone && bone->skinIndex == skinIndex &&
                    (bone->paletteIndex < 0 ||
                        bone->paletteIndex == static_cast<int>(palette)))
                {
                    m_cachedBones[palette] = bone;
                    break;
                }
    return m_cachedBones;
}

const std::vector<AnimationBone*>& Skeleton::ResolveRootBones() const
{
    m_cachedRootBones.clear();
    for (AnimationBone* bone : ResolveBones())
        if (bone && !bone->GetParentBone())
            m_cachedRootBones.push_back(bone);
    return m_cachedRootBones;
}
}
