#include "SkinnedMesh.h"
#include "Skeleton.h"
#include "Core/Object.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <vector>

namespace Engine::Components
{
namespace
{
glm::mat4 LocalMatrix(const Engine::Core::Object& object)
{
    const auto& transform = object.transform;
    return glm::translate(glm::mat4(1.f), transform.position) *
        glm::rotate(glm::mat4(1.f), transform.rotation.z, { 0.f, 0.f, 1.f }) *
        glm::rotate(glm::mat4(1.f), transform.rotation.y, { 0.f, 1.f, 0.f }) *
        glm::rotate(glm::mat4(1.f), transform.rotation.x, { 1.f, 0.f, 0.f }) *
        glm::scale(glm::mat4(1.f), transform.scale);
}

// Skinning asks for the world matrix of every joint. Calling
// Transform::GetWorldMatrix for each one walks and rebuilds the shared parent
// chain repeatedly, which is quadratic for the long chains common in rigs.
// Cache ancestors for this palette build so every local transform is composed
// at most once while still reflecting live gizmo edits in the same frame.
glm::mat4 CachedWorldMatrix(Engine::Core::Object* object,
    std::unordered_map<Engine::Core::Object*, glm::mat4>& cache)
{
    if (!object) return glm::mat4(1.f);
    if (const auto found = cache.find(object); found != cache.end())
        return found->second;

    std::vector<Engine::Core::Object*> chain;
    Engine::Core::Object* current = object;
    while (current && cache.find(current) == cache.end())
    {
        chain.push_back(current);
        current = current->Parent;
    }

    glm::mat4 world = current ? cache.find(current)->second : glm::mat4(1.f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        world *= LocalMatrix(**it);
        cache.emplace(*it, world);
    }
    return cache.find(object)->second;
}
}

SkinnedMesh::SkinnedMesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(SkinnedMesh));
    RegisterField("meshReference", meshReference);
    RegisterField("skeletonReference", skeletonReference);
}

void SkinnedMesh::Start()
{
    Mesh* mesh = Owner ? (meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
    if (!mesh) return;
    m_baseVertices = mesh->GetVertices();
    // Compatibility with prefabs imported before influences moved into Mesh.
    if (joints.size() == m_baseVertices.size() &&
        weights.size() == m_baseVertices.size())
    {
        bool changed = false;
        for (size_t vertexIndex = 0; vertexIndex < m_baseVertices.size(); ++vertexIndex)
        {
            float existingWeight = 0.f;
            for (size_t influence = 0; influence < 4; ++influence)
                existingWeight += m_baseVertices[vertexIndex].weights0[influence];
            if (existingWeight > 0.f) continue;
            for (size_t influence = 0; influence < 4; ++influence)
            {
                m_baseVertices[vertexIndex].joints0[influence] =
                    static_cast<float>(joints[vertexIndex][static_cast<glm::length_t>(influence)]);
                m_baseVertices[vertexIndex].weights0[influence] =
                    weights[vertexIndex][static_cast<glm::length_t>(influence)];
            }
            changed = true;
        }
        if (changed) mesh->SetDeformedVertices(m_baseVertices);
    }
}

void SkinnedMesh::Update()
{
    Mesh* mesh = Owner ? (meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
    if (!mesh) return;
    if (!mesh->HasMorphTargets()) return;
    if (m_baseVertices.size() != mesh->GetVertices().size())
        m_baseVertices = mesh->GetVertices();
    std::vector<Vertex> deformed = m_baseVertices;
    const auto& morphTargets = mesh->GetMorphTargets();
    const auto& morphWeights = mesh->GetMorphWeights();
    for (size_t targetIndex = 0; targetIndex < morphTargets.size(); ++targetIndex)
    {
        const float weight = targetIndex < morphWeights.size()
            ? morphWeights[targetIndex] : 0.f;
        if (weight == 0.f) continue;
        const auto& target = morphTargets[targetIndex];
        for (size_t i = 0; i < deformed.size(); ++i)
        {
            if (i < target.positions.size())
            {
                deformed[i].pos[0] += target.positions[i].x * weight;
                deformed[i].pos[1] += target.positions[i].y * weight;
                deformed[i].pos[2] += target.positions[i].z * weight;
            }
            if (i < target.normals.size())
            {
                deformed[i].normal[0] += target.normals[i].x * weight;
                deformed[i].normal[1] += target.normals[i].y * weight;
                deformed[i].normal[2] += target.normals[i].z * weight;
            }
            if (i < target.tangents.size())
            {
                deformed[i].tangent[0] += target.tangents[i].x * weight;
                deformed[i].tangent[1] += target.tangents[i].y * weight;
                deformed[i].tangent[2] += target.tangents[i].z * weight;
            }
        }
    }
    for (Vertex& vertex : deformed)
    {
        glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (glm::dot(normal, normal) > 0.000001f)
        {
            normal = glm::normalize(normal);
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
        }
        glm::vec3 tangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
        if (glm::dot(tangent, tangent) > 0.000001f)
        {
            tangent = glm::normalize(tangent);
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
        }
    }
    mesh->SetDeformedVertices(deformed);
}

bool SkinnedMesh::BuildPalette(std::vector<glm::mat4>& palette) const
{
    palette.clear();
    if (!Owner || skinIndex < 0) return false;
    Skeleton* skeleton = skeletonReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Skeleton>(Owner, skeletonReference) : nullptr;
    if (!skeletonReference.IsAssigned())
        for (Object* ancestor = Owner; ancestor && !skeleton; ancestor = ancestor->Parent)
            for (Component* component : ancestor->Components)
                if (auto* candidate = dynamic_cast<Skeleton*>(component);
                    candidate && candidate->skinIndex == static_cast<unsigned>(skinIndex))
                {
                    skeleton = candidate;
                    break;
                }
    if (!skeleton) return false;
    palette.assign(skeleton->jointNodes.size(), glm::mat4(1.f));
    Mesh* mesh = meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>();
    Object* meshObject = mesh && mesh->Owner ? mesh->Owner : Owner;
    std::unordered_map<Engine::Core::Object*, glm::mat4> worldMatrices;
    worldMatrices.reserve(skeleton->jointNodes.size() * 2 + 1);
    const glm::mat4 inverseMesh = glm::inverse(
        CachedWorldMatrix(meshObject, worldMatrices));
    const std::vector<Object*> resolvedJoints = skeleton->ResolveJoints();
    for (size_t i = 0; i < resolvedJoints.size(); ++i)
        if (Object* joint = resolvedJoints[i])
            palette[i] = inverseMesh * CachedWorldMatrix(joint, worldMatrices) *
                (i < skeleton->inverseBindMatrices.size()
                    ? skeleton->inverseBindMatrices[i] : glm::mat4(1.f));
    return !palette.empty();
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
