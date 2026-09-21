#include "Skeleton.h"
#include "Model.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Components
{
Skeleton::Skeleton()
{
    SetTypeName(COMPONENT_TYPE_NAME(Skeleton));
    RegisterField("modelReference", modelReference);
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
    const char* label = showBones ? "Hide Bones in Scene" : "Show Bones in Scene";
    if (ui.Button(label))
    {
        showBones = !showBones;
        return true;
    }
    const std::string skin = std::to_string(skinIndex);
    const std::string bones = std::to_string(jointNodes.size());
    ui.ValueLabel("Skin", skin.c_str());
    ui.ValueLabel("Bones", bones.c_str());
    return false;
}

Skeleton::Object* Skeleton::FindNode(unsigned index) const
{
    Model* model = ResolveModel();
    return model ? model->ResolveNode(index) : nullptr;
}

Skeleton::Object* Skeleton::GetHierarchyRoot() const
{
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
}
