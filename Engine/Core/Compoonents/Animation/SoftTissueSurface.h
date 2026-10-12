#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Engine::Components
{
class SkinnedMesh;
class Skeleton;
class MeshObjectCollider;

// A bounded secondary surface layer evaluated in mesh-local posed space.
class SoftTissueSurface final : public Engine::Core::Component
{
public:
    SoftTissueSurface();
    ComponentReference meshReference { "Mesh" };
    ComponentReference skinReference { "SkinnedMesh" };
    ComponentReference colliderReference { "MeshObjectCollider" };
    float poseStrength = 0.015f;
    float impulseScale = 0.015f;
    float impulseThreshold = 0.05f;
    float maxContactImpulse = 12.f;
    float propagationRadius = 0.2f;
    float attachmentFrequency = 8.f;
    float damping = 1.f;
    float maxDisplacement = 0.04f;
    float maxEdgeStrain = 0.35f;
    unsigned maxActiveVertices = 8192;
    unsigned maxSimulatedVertices = 100000;
    unsigned maxContacts = 16;
    float maxSimulationDistance = 0.f; // Zero keeps the response active at any distance.
    // Independent response mask; absent entries default to one.
    std::vector<float> responseMask;

    void Start() override;
    void OnDestroy() override;
    void Disabled() override { Reset(); }
    void Enabled() override { Start(); }
    void PostPhysicsUpdate(float fixedDelta);
    // For mapped impulses produced outside the bound Bullet collider.
    void QueueMappedContact(const MeshObjectCollider::Contact& contact);
    void Reset();
    bool IsBoundUnder(const Engine::Core::Object* root) const;
    const std::vector<glm::vec3>& GetOffsets() const { return m_offset; }
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

private:
    void Bind();
    Mesh* ResolveMesh() const;
    SkinnedMesh* ResolveSkin() const;
    MeshObjectCollider* ResolveCollider() const;
    Mesh* m_mesh = nullptr;
    SkinnedMesh* m_skin = nullptr;
    Skeleton* m_skeleton = nullptr;
    uint64_t m_geometryRevision = 0;
    uint64_t m_skinRevision = 0;
    uint64_t m_skeletonRevision = 0;
    std::vector<std::vector<uint32_t>> m_neighbors;
    std::vector<glm::vec3> m_offset;
    std::vector<glm::vec3> m_velocity;
    std::unordered_set<uint64_t> m_previousContacts;
    std::vector<glm::vec3> m_previousContactPoints;
    std::vector<MeshObjectCollider::Contact> m_queuedContacts;
    glm::vec3 m_previousWorldPosition { 0.f };
    bool m_hasPreviousWorldPosition = false;
};
}
