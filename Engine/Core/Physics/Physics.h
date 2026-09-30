#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Core { class Object; }
namespace Engine::Components { class RigidBody; }

namespace Engine::Physics
{
class Physics
{
public:
    enum class RaycastHitKind : uint8_t
    {
        RigidBody,
        PortalSplitPiece,
        PortalApertureRim
    };
    struct RaycastHit
    {
        RaycastHitKind kind = RaycastHitKind::RigidBody;
        Engine::Core::Object* object = nullptr;
        Engine::Components::RigidBody* rigidBody = nullptr;
        const void* instanceKey = nullptr;
        glm::vec3 point { 0.f };
        glm::vec3 normal { 0.f, 1.f, 0.f };
        float distance = 0.f;
    };

    explicit Physics(Engine::Scene::Scene& scene);
    ~Physics();

    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    void Step(float deltaTime);
    void Reset();

    // Runtime-only portal collision instances. They represent the remote
    // portion of a mesh while an object is split across a connection. The
    // remote piece is a target-space BvhTriangleMesh collision object, so it
    // participates in ordinary Bullet contact and ray queries; its owning
    // local rigid body is explicitly filtered out. Callers that traverse a
    // portal ray map the ray through the connection before querying this
    // target-space geometry.
    void SetPortalMeshCollider(const void* instanceKey,
        const Engine::Components::RigidBody& owner,
        const std::vector<glm::vec3>& worldVertices);
    void RemovePortalMeshCollider(const void* instanceKey);
    size_t GetPortalMeshColliderCount(
        const Engine::Components::RigidBody& owner) const;

    // Static, solid aperture rim. It has no centre face: bodies may pass
    // through the opening but collide with its polygon edges.
    void SetPortalApertureCollider(const void* instanceKey,
        const std::vector<glm::vec3>& worldPoints,
        const glm::vec3& worldNormal, float edgeHalfWidth,
        float edgeHalfDepth);
    void RemovePortalApertureCollider(const void* instanceKey);

    // Returns every Bullet hit in ascending distance order. Portal-aware
    // callers use this primitive for each straight chart segment.
    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin,
        const glm::vec3& direction, float maxDistance,
        uint32_t collisionMask = ~0u) const;

    // Internal bridge for physics components; keeps Bullet types out of the
    // engine-facing header.
    void* GetInternalState();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
}
