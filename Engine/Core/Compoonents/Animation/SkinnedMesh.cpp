#include "SkinnedMesh.h"
#include "Skeleton.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace Engine::Components
{
void SkinnedMesh::ResolveBindings() const
{
    const uint64_t structureRevision = Owner && Owner->GetScene()
        ? Owner->GetScene()->GetStructureRevision() : 0;
    const uint64_t configurationRevision = GetConfigurationRevision();
    // Prefab components can run OnAfterDeserialize before the complete parent
    // hierarchy has been attached. Scene structure revisions do not always
    // change for that assembly step, so do not keep an unresolved binding as a
    // permanent cache hit.
    const bool bindingsResolved = m_cachedMesh &&
        (skinIndex < 0 || m_cachedSkeleton);
    if (m_bindingCacheValid && bindingsResolved &&
        m_cachedStructureRevision == structureRevision &&
        m_cachedConfigurationRevision == configurationRevision &&
        m_cachedSkinIndex == skinIndex)
        return;

    m_cachedMesh = Owner ? (meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
    m_cachedSkeleton = Owner && skeletonReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Skeleton>(Owner, skeletonReference) : nullptr;
    if (Owner && skinIndex >= 0 && !skeletonReference.IsAssigned())
        for (Object* ancestor = Owner; ancestor && !m_cachedSkeleton;
            ancestor = ancestor->Parent)
            for (Component* component : ancestor->Components)
                if (auto* candidate = dynamic_cast<Skeleton*>(component);
                    candidate && candidate->skinIndex == static_cast<unsigned>(skinIndex))
                {
                    m_cachedSkeleton = candidate;
                    break;
                }
    m_cachedStructureRevision = structureRevision;
    m_cachedConfigurationRevision = configurationRevision;
    m_cachedSkinIndex = skinIndex;
    m_bindingCacheValid = true;
}

SkinnedMesh::SkinnedMesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(SkinnedMesh));
    RegisterField("meshReference", meshReference);
    RegisterField("skeletonReference", skeletonReference);
}

bool SkinnedMesh::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    bool changed = false;
    changed = DrawReferenceProperty(ui, "meshReference", "Mesh",
        meshReference) || changed;
    Mesh* mesh = Owner ? (meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;

    Skeleton* skeleton = Owner && skeletonReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Skeleton>(
            Owner, skeletonReference) : nullptr;
    if (skinIndex >= 0 && !skeletonReference.IsAssigned())
        for (Object* ancestor = Owner; ancestor && !skeleton;
            ancestor = ancestor->Parent)
            for (Component* component : ancestor->Components)
                if (auto* candidate = dynamic_cast<Skeleton*>(component);
                    candidate && candidate->skinIndex == static_cast<unsigned>(skinIndex))
                {
                    skeleton = candidate;
                    break;
                }
    changed = DrawReferenceProperty(ui, "skeletonReference", "Skeleton",
        skeletonReference, skeleton) || changed;

    float editedSkin = static_cast<float>(skinIndex);
    if (ui.DragFloat("Skin Index", &editedSkin, 1.f, -1.f, 10000.f))
    {
        skinIndex = static_cast<int>(std::round(editedSkin));
        changed = true;
    }
    const std::string jointEntries = std::to_string(joints.size());
    const std::string weightEntries = std::to_string(weights.size());
    const std::string baseVertices = mesh
        ? std::to_string(mesh->GetVertices().size()) : "0";
    const std::string resolvedBones = skeleton
        ? std::to_string(skeleton->jointNodes.size()) : "0";
    ui.ValueLabel("Joint Entries", jointEntries.c_str());
    ui.ValueLabel("Weight Entries", weightEntries.c_str());
    ui.ValueLabel("Base Vertices", baseVertices.c_str());
    ui.ValueLabel("Resolved Bones", resolvedBones.c_str());
    ui.DisabledLabel("Per-vertex joint and weight arrays are read-only here.");

    if (changed)
    {
        MarkConfigurationDirty();
        Start();
    }
    return changed;
}

void SkinnedMesh::Start()
{
    ResolveBindings();
    Mesh* mesh = m_cachedMesh;
    if (!mesh) return;
    std::vector<Vertex> baseVertices = mesh->GetVertices();
    // Compatibility with prefabs imported before influences moved into Mesh.
    if (joints.size() == baseVertices.size() &&
        weights.size() == baseVertices.size())
    {
        bool changed = false;
        for (size_t vertexIndex = 0; vertexIndex < baseVertices.size(); ++vertexIndex)
        {
            float existingWeight = 0.f;
            for (size_t influence = 0; influence < 4; ++influence)
                existingWeight += baseVertices[vertexIndex].weights0[influence];
            if (existingWeight > 0.f) continue;
            for (size_t influence = 0; influence < 4; ++influence)
            {
                baseVertices[vertexIndex].joints0[influence] =
                    static_cast<float>(joints[vertexIndex][static_cast<glm::length_t>(influence)]);
                baseVertices[vertexIndex].weights0[influence] =
                    weights[vertexIndex][static_cast<glm::length_t>(influence)];
            }
            changed = true;
        }
        if (changed) mesh->SetDeformedVertices(std::move(baseVertices));
    }
}

void SkinnedMesh::Update()
{
    // Morph positions, normals, and tangents are blended from persistent
    // structured buffers by the object vertex shader. Animation changes only
    // the mesh's compact GPU weight buffer; the authored CPU vertex stream is
    // deliberately left immutable here. Sync also preserves support for tools
    // that edit the mutable weight array directly instead of SetMorphWeights.
    ResolveBindings();
    Mesh* mesh = m_cachedMesh;
    if (mesh)
        mesh->SyncMorphWeights();
}

const std::vector<glm::mat4>& SkinnedMesh::BuildPalette() const
{
    m_palette.clear();
    if (!Owner || skinIndex < 0) return m_palette;
    ResolveBindings();
    Skeleton* skeleton = m_cachedSkeleton;
    if (!skeleton) return m_palette;
    m_palette.assign(skeleton->jointNodes.size(), glm::mat4(1.f));
    Mesh* mesh = m_cachedMesh;
    Object* meshObject = mesh && mesh->Owner ? mesh->Owner : Owner;
    const glm::mat4 inverseMesh = glm::inverse(
        meshObject->transform.GetWorldMatrix());
    const std::vector<Object*>& resolvedJoints = skeleton->ResolveJoints();
    for (size_t i = 0; i < resolvedJoints.size(); ++i)
        if (Object* joint = resolvedJoints[i])
            m_palette[i] = inverseMesh * joint->transform.GetWorldMatrix() *
                (i < skeleton->inverseBindMatrices.size()
                    ? skeleton->inverseBindMatrices[i] : glm::mat4(1.f));
    return m_palette;
}

Skeleton* SkinnedMesh::ResolveSkeleton() const
{
    ResolveBindings();
    return m_cachedSkeleton;
}

SkinnedMesh::JsonValue SkinnedMesh::Serialize() const
{
    JsonValue result = Component::Serialize().Set("skinIndex", JsonValue(skinIndex));
    JsonValue serializedJoints = JsonValue::MakeArray();
    JsonValue serializedWeights = JsonValue::MakeArray();
    for (size_t i = 0; i < joints.size(); ++i)
    {
        serializedJoints.Push(JsonValue::MakeArray()
            .Push(JsonValue(static_cast<int>(joints[i].x)))
            .Push(JsonValue(static_cast<int>(joints[i].y)))
            .Push(JsonValue(static_cast<int>(joints[i].z)))
            .Push(JsonValue(static_cast<int>(joints[i].w))));
        serializedWeights.Push(JsonValue::MakeArray()
            .Push(JsonValue(weights[i].x)).Push(JsonValue(weights[i].y))
            .Push(JsonValue(weights[i].z)).Push(JsonValue(weights[i].w)));
    }
    return result.Set("joints", std::move(serializedJoints))
        .Set("weights", std::move(serializedWeights));
}

void SkinnedMesh::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    skinIndex = value["skinIndex"].AsInt();
    joints.clear();
    weights.clear();
    for (size_t i = 0; i < value["joints"].ArraySize(); ++i)
    {
        const JsonValue& joint = value["joints"].ArrayAt(i);
        const JsonValue& weight = value["weights"].ArrayAt(i);
        joints.emplace_back(joint.ArrayAt(0).AsInt(), joint.ArrayAt(1).AsInt(),
            joint.ArrayAt(2).AsInt(), joint.ArrayAt(3).AsInt());
        weights.emplace_back(weight.ArrayAt(0).AsFloat(), weight.ArrayAt(1).AsFloat(),
            weight.ArrayAt(2).AsFloat(), weight.ArrayAt(3).AsFloat());
    }
}
}
