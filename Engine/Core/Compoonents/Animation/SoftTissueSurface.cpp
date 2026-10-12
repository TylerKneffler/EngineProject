#include "SoftTissueSurface.h"
#include "SkinnedMesh.h"
#include "Skeleton.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <string>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Components
{
namespace
{
glm::vec3 Position(const Mesh::Vertex& vertex)
{
    return { vertex.pos[0], vertex.pos[1], vertex.pos[2] };
}

glm::vec3 Normal(const Mesh::Vertex& vertex)
{
    const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
    return glm::dot(normal, normal) > 1e-12f
        ? glm::normalize(normal) : glm::vec3(0.f, 1.f, 0.f);
}

bool Finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}
}

SoftTissueSurface::SoftTissueSurface()
{
    SetTypeName(COMPONENT_TYPE_NAME(SoftTissueSurface));
    RegisterField("meshReference", meshReference);
    RegisterField("skinReference", skinReference);
    RegisterField("colliderReference", colliderReference);
    RegisterField("poseStrength", poseStrength, "Response");
    RegisterField("impulseScale", impulseScale, "Response");
    RegisterField("impulseThreshold", impulseThreshold, "Response");
    RegisterField("maxContactImpulse", maxContactImpulse, "Response");
    RegisterField("propagationRadius", propagationRadius, "Response");
    RegisterField("attachmentFrequency", attachmentFrequency, "Solver");
    RegisterField("damping", damping, "Solver");
    RegisterField("maxDisplacement", maxDisplacement, "Solver");
    RegisterField("maxEdgeStrain", maxEdgeStrain, "Solver");
    RegisterField("maxActiveVertices", maxActiveVertices, "Budget");
    RegisterField("maxSimulatedVertices", maxSimulatedVertices, "Budget");
    RegisterField("maxContacts", maxContacts, "Budget");
    RegisterField("maxSimulationDistance", maxSimulationDistance, "Budget");
}

Mesh* SoftTissueSurface::ResolveMesh() const
{
    return Owner ? (meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : Owner->GetComponent<Mesh>()) : nullptr;
}

SkinnedMesh* SoftTissueSurface::ResolveSkin() const
{
    return Owner ? (skinReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<SkinnedMesh>(Owner, skinReference)
        : Owner->GetComponent<SkinnedMesh>()) : nullptr;
}

MeshObjectCollider* SoftTissueSurface::ResolveCollider() const
{
    return Owner ? (colliderReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<MeshObjectCollider>(Owner, colliderReference)
        : Owner->GetComponent<MeshObjectCollider>()) : nullptr;
}

bool SoftTissueSurface::IsBoundUnder(const Engine::Core::Object* root) const
{
    const Mesh* mesh = ResolveMesh();
    for (const Engine::Core::Object* object = mesh ? mesh->Owner : nullptr;
         object; object = object->Parent)
        if (object == root) return true;
    return false;
}

void SoftTissueSurface::Start()
{
    Reset();
    Bind();
}

void SoftTissueSurface::OnDestroy()
{
    // Scene teardown detaches all objects before destroying them. A referenced
    // mesh may already be gone by the time this component is destroyed.
    if (Owner && Owner->GetScene()) Reset();
    else m_mesh = nullptr;
}

void SoftTissueSurface::Reset()
{
    if (m_mesh) m_mesh->ClearSurfaceOffsets();
    m_mesh = nullptr;
    m_skin = nullptr;
    m_skeleton = nullptr;
    m_geometryRevision = 0;
    m_skinRevision = 0;
    m_skeletonRevision = 0;
    m_neighbors.clear();
    m_offset.clear();
    m_velocity.clear();
    m_previousContacts.clear();
    m_previousContactPoints.clear();
    m_queuedContacts.clear();
    m_hasPreviousWorldPosition = false;
}

void SoftTissueSurface::Bind()
{
    Mesh* mesh = ResolveMesh();
    SkinnedMesh* skin = ResolveSkin();
    if (!mesh || !skin || mesh->GetVertices().empty() ||
        mesh->UsesTerrainVertexFormat() ||
        mesh->GetVertices().size() > maxSimulatedVertices) return;
    m_mesh = mesh;
    m_skin = skin;
    m_skeleton = skin->ResolveSkeleton();
    m_geometryRevision = mesh->GetAuthoredGeometryRevision();
    m_skinRevision = skin->GetConfigurationRevision();
    m_skeletonRevision = m_skeleton ? m_skeleton->GetConfigurationRevision() : 0;
    const size_t count = mesh->GetVertices().size();
    if (!responseMask.empty() && responseMask.size() != count)
        responseMask.clear();
    m_offset.assign(count, glm::vec3(0.f));
    m_velocity.assign(count, glm::vec3(0.f));
    m_neighbors.resize(count);
    const auto& indices = mesh->GetIndices();
    const size_t triangles = indices.empty() ? count / 3 : indices.size() / 3;
    for (size_t triangle = 0; triangle < triangles; ++triangle)
    {
        const uint32_t ids[3] = {
            indices.empty() ? static_cast<uint32_t>(triangle * 3) : indices[triangle * 3],
            indices.empty() ? static_cast<uint32_t>(triangle * 3 + 1) : indices[triangle * 3 + 1],
            indices.empty() ? static_cast<uint32_t>(triangle * 3 + 2) : indices[triangle * 3 + 2] };
        for (unsigned corner = 0; corner < 3; ++corner)
            if (ids[corner] < count && ids[(corner + 1) % 3] < count)
            {
                m_neighbors[ids[corner]].push_back(ids[(corner + 1) % 3]);
                m_neighbors[ids[(corner + 1) % 3]].push_back(ids[corner]);
            }
    }
    // Triangle-list meshes duplicate vertices. Join only identical position,
    // UV and influence records; UV or skinning seams remain independent.
    if (indices.empty())
    {
        std::unordered_map<std::string, uint32_t> firstVertex;
        const auto& vertices = mesh->GetVertices();
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& vertex = vertices[index];
            std::string key;
            const auto append = [&](const float* values, size_t length) {
                key.append(reinterpret_cast<const char*>(values),
                    length * sizeof(float));
            };
            append(vertex.pos, 3);
            append(vertex.uv, 2);
            append(vertex.joints0, 4);
            append(vertex.weights0, 4);
            append(vertex.joints1, 4);
            append(vertex.weights1, 4);
            const auto [found, inserted] = firstVertex.emplace(std::move(key), index);
            if (!inserted)
            {
                m_neighbors[index].push_back(found->second);
                m_neighbors[found->second].push_back(index);
            }
        }
    }
    for (auto& neighbors : m_neighbors)
    {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
}

void SoftTissueSurface::QueueMappedContact(
    const MeshObjectCollider::Contact& contact)
{
    if (contact.surfaceMapped && m_queuedContacts.size() < maxContacts)
        m_queuedContacts.push_back(contact);
}

void SoftTissueSurface::PostPhysicsUpdate(float fixedDelta)
{
    if (!std::isfinite(fixedDelta) || fixedDelta > 0.2f)
    {
        Reset();
        return;
    }
    Mesh* mesh = ResolveMesh();
    SkinnedMesh* skin = ResolveSkin();
    if (mesh && mesh->GetVertices().size() > maxSimulatedVertices)
    {
        Reset();
        return;
    }
    if (mesh != m_mesh || skin != m_skin || !mesh || !skin ||
        mesh->GetAuthoredGeometryRevision() != m_geometryRevision ||
        skin->GetConfigurationRevision() != m_skinRevision ||
        skin->ResolveSkeleton() != m_skeleton ||
        (m_skeleton && m_skeleton->GetConfigurationRevision() != m_skeletonRevision) ||
        mesh->GetVertices().size() != m_offset.size())
    {
        Reset();
        Bind();
    }
    if (!m_mesh || !m_skin || !std::isfinite(fixedDelta) ||
        fixedDelta <= 0.f) return;
    if (maxSimulationDistance > 0.f && Owner && Owner->GetScene())
        if (auto* camera = Owner->GetScene()->FindGameCamera())
        {
            const glm::vec3 surfacePosition = glm::vec3(
                m_mesh->Owner->transform.GetWorldMatrix()[3]);
            const glm::vec3 cameraPosition = glm::vec3(
                camera->Owner->transform.GetWorldMatrix()[3]);
            if (glm::distance(surfacePosition, cameraPosition) >
                maxSimulationDistance)
            {
                std::fill(m_offset.begin(), m_offset.end(), glm::vec3(0.f));
                std::fill(m_velocity.begin(), m_velocity.end(), glm::vec3(0.f));
                m_previousContacts.clear();
                m_previousContactPoints.clear();
                m_mesh->ClearSurfaceOffsets();
                return;
            }
        }
    const glm::vec3 worldPosition = glm::vec3(
        m_mesh->Owner->transform.GetWorldMatrix()[3]);
    if (m_hasPreviousWorldPosition &&
        glm::length(worldPosition - m_previousWorldPosition) > 1.f)
    {
        std::fill(m_offset.begin(), m_offset.end(), glm::vec3(0.f));
        std::fill(m_velocity.begin(), m_velocity.end(), glm::vec3(0.f));
        m_previousContacts.clear();
        m_previousContactPoints.clear();
    }
    m_previousWorldPosition = worldPosition;
    m_hasPreviousWorldPosition = true;

    const auto& palette = m_skin->BuildPalette();
    const auto posed = m_mesh->BuildPosedVertices(&palette, false);
    const auto& rest = m_mesh->GetVertices();
    if (posed.size() != rest.size()) return;
    const float dt = std::min(fixedDelta, 1.f / 30.f);
    const float limit = std::clamp(maxDisplacement, 0.f, 10.f);
    const float frequency = std::clamp(attachmentFrequency, 0.1f, 60.f);
    const float dampingRatio = std::clamp(damping, 0.f, 10.f);
    const auto mask = [&](size_t index) {
        return index < responseMask.size()
            ? std::clamp(responseMask[index], 0.f, 1.f) : 1.f;
    };

    std::vector<glm::vec3> target(posed.size(), glm::vec3(0.f));
    for (size_t index = 0; index < posed.size(); ++index)
    {
        float strain = 0.f;
        unsigned edges = 0;
        for (uint32_t neighbor : m_neighbors[index])
        {
            const float restLength = glm::length(Position(rest[index]) -
                Position(rest[neighbor]));
            if (restLength <= 1e-5f) continue;
            const float posedLength = glm::length(Position(posed[index]) -
                Position(posed[neighbor]));
            strain += std::clamp((posedLength - restLength) / restLength,
                -1.f, 1.f);
            ++edges;
        }
        if (edges)
            target[index] = Normal(posed[index]) *
                std::clamp(poseStrength, 0.f, limit) *
                (strain / static_cast<float>(edges)) * mask(index);
    }

    std::unordered_set<uint64_t> currentContacts;
    std::vector<glm::vec3> currentContactPoints;
    std::vector<MeshObjectCollider::Contact> contacts = std::move(m_queuedContacts);
    m_queuedContacts.clear();
    if (const auto* collider = ResolveCollider())
        contacts.insert(contacts.end(), collider->GetContacts().begin(),
            collider->GetContacts().end());
    if (!contacts.empty())
    {
        unsigned consumed = 0;
        const glm::mat3 inverseWorld = glm::mat3(glm::inverse(
            m_mesh->Owner->transform.GetWorldMatrix()));
        const auto& indices = m_mesh->GetIndices();
        for (const auto& contact : contacts)
        {
            if (!contact.surfaceMapped || consumed >= maxContacts) continue;
            const float impulse = std::clamp(contact.normalImpulse /
                std::max(1u, static_cast<unsigned>(contact.substepSamples)),
                0.f, std::max(0.f, maxContactImpulse));
            if (impulse < std::max(0.f, impulseThreshold)) continue;
            currentContacts.insert(contact.contactId);
            currentContactPoints.push_back(contact.surfacePointWorld);
            bool resting = m_previousContacts.count(contact.contactId) != 0;
            for (const glm::vec3& previous : m_previousContactPoints)
                if (glm::distance(previous, contact.surfacePointWorld) < 0.015f)
                    resting = true;
            if (resting) continue;
            const size_t base = static_cast<size_t>(contact.triangleIndex) * 3;
            if (base + 2 >= (indices.empty() ? rest.size() : indices.size())) continue;
            glm::vec3 direction = inverseWorld * contact.normalWorld;
            if (!Finite(direction) || glm::dot(direction, direction) < 1e-12f)
                continue;
            direction = glm::normalize(direction);
            std::deque<std::pair<uint32_t, float>> queue;
            std::unordered_map<uint32_t, float> distance;
            std::unordered_map<uint32_t, float> seedWeight;
            for (unsigned corner = 0; corner < 3; ++corner)
            {
                if (contact.barycentric[corner] <= 0.01f) continue;
                const uint32_t id = indices.empty()
                    ? static_cast<uint32_t>(base + corner) : indices[base + corner];
                if (id >= rest.size()) continue;
                distance[id] = 0.f;
                seedWeight[id] = std::min(1.f, seedWeight[id] +
                    3.f * contact.barycentric[corner]);
                queue.emplace_back(id, 0.f);
            }
            unsigned active = 0;
            const float radius = std::max(propagationRadius, 1e-4f);
            while (!queue.empty() && active < maxActiveVertices)
            {
                const auto [id, path] = queue.front();
                queue.pop_front();
                if (path > distance[id] + 1e-5f) continue;
                const float falloff = path == 0.f ? seedWeight[id]
                    : 1.f - std::clamp(path / radius, 0.f, 1.f);
                m_velocity[id] += direction * std::min(limit,
                    impulse * std::max(0.f, impulseScale)) * falloff * mask(id);
                ++active;
                for (uint32_t neighbor : m_neighbors[id])
                {
                    const float next = path + glm::length(
                        Position(posed[id]) - Position(posed[neighbor]));
                    const auto prior = distance.find(neighbor);
                    if (next < radius && (prior == distance.end() ||
                        next < prior->second))
                    {
                        distance[neighbor] = next;
                        queue.emplace_back(neighbor, next);
                    }
                }
            }
            ++consumed;
        }
    }
    m_previousContacts = std::move(currentContacts);
    m_previousContactPoints = std::move(currentContactPoints);

    for (size_t index = 0; index < m_offset.size(); ++index)
    {
        const float denominator = 1.f + 2.f * dampingRatio * frequency * dt +
            frequency * frequency * dt * dt;
        m_velocity[index] = (m_velocity[index] +
            (target[index] - m_offset[index]) *
            (frequency * frequency * dt)) / denominator;
        m_offset[index] += m_velocity[index] * dt;
        const float length = glm::length(m_offset[index]);
        if (!Finite(m_offset[index]) || !Finite(m_velocity[index]))
            m_offset[index] = m_velocity[index] = glm::vec3(0.f);
        else if (length > limit && length > 0.f)
        {
            m_offset[index] *= limit / length;
            m_velocity[index] = glm::vec3(0.f);
        }
    }
    const float strainLimit = std::clamp(maxEdgeStrain, 0.f, 1.f);
    for (size_t index = 0; index < m_offset.size(); ++index)
        for (uint32_t neighbor : m_neighbors[index])
            if (neighbor > index)
            {
                const float edgeLength = glm::length(
                    Position(rest[index]) - Position(rest[neighbor]));
                const glm::vec3 difference = m_offset[index] - m_offset[neighbor];
                const float length = glm::length(difference);
                const float allowed = edgeLength * strainLimit;
                if (length > allowed && length > 1e-8f)
                {
                    const glm::vec3 correction = difference *
                        ((length - allowed) / length * 0.5f);
                    m_offset[index] -= correction;
                    m_offset[neighbor] += correction;
                }
            }
    for (glm::vec3& offset : m_offset)
    {
        const float length = glm::length(offset);
        if (!Finite(offset)) offset = glm::vec3(0.f);
        else if (length > limit && length > 0.f) offset *= limit / length;
    }
    m_mesh->SetSurfaceOffsets(m_offset, &palette);
}

SoftTissueSurface::JsonValue SoftTissueSurface::Serialize() const
{
    JsonValue value = Component::Serialize();
    JsonValue mask = JsonValue::MakeArray();
    for (float weight : responseMask) mask.Push(JsonValue(weight));
    value.Set("responseMask", std::move(mask));
    return value;
}

void SoftTissueSurface::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    responseMask.clear();
    for (size_t index = 0; index < value["responseMask"].ArraySize(); ++index)
        responseMask.push_back(std::clamp(
            value["responseMask"].ArrayAt(index).AsFloat(), 0.f, 1.f));
    Reset();
}
}
