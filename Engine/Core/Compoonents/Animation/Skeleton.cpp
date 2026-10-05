#include "Skeleton.h"
#include "AnimationBone.h"
#include "Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Components
{
Skeleton::Skeleton()
{
    SetTypeName(COMPONENT_TYPE_NAME(Skeleton));
    RegisterField("modelReference", modelReference);
    RegisterField("colliderMode", colliderMode, "Collision");
    RegisterField("meshColliderReference", meshColliderReference, "Collision");
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
}

bool Skeleton::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    changed = DrawReferenceProperty(ui, "modelReference", "Model",
        modelReference, ResolveModel()) || changed;
    const bool wholeMesh = UsesWholeMeshCollider();
    if (ui.Button(wholeMesh ? "Collision: Whole Mesh" : "Collision: Per Bone"))
    {
        colliderMode = wholeMesh ? "PerBone" : "WholeMesh";
        changed = true;
    }
    if (UsesWholeMeshCollider())
    {
        changed = DrawReferenceProperty(ui, "meshColliderReference",
            "Mesh Collider", meshColliderReference) || changed;
        MeshObjectCollider* collider = ResolveMeshCollider();
        if (!collider)
            ui.DisabledLabel("Assign a MeshObjectCollider to enable whole-mesh collision.");
        else if (!collider->Owner ||
            !collider->Owner->GetComponent<RigidBody>())
            ui.DisabledLabel("Add a Kinematic RigidBody to the collider object.");
        else if (collider->Owner->GetComponent<RigidBody>()->bodyType == "Dynamic")
            ui.DisabledLabel("Whole-mesh collision requires a Kinematic or Static body.");
        else
            ui.DisabledLabel("Mesh collider uses the current skin and morph pose.");
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

bool Skeleton::UsesWholeMeshCollider() const
{
    return colliderMode == "WholeMesh";
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
