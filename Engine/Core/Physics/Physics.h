#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Core { class Object; }
namespace Engine::Components { class RigidBody; }
namespace Engine::Components { class Cloth; class IKBone; }

namespace Engine::Physics
{
class IPhysicsAdapter;
enum class RigidBodyAction : uint8_t
{
    AddForce, AddTorque, AddImpulse, AddAngularImpulse,
    SetWorldPosition, SetWorldPose, SetLinearVelocity,
    SetAngularVelocity, SetGravityDirection
};
enum class RigidBodyVector : uint8_t
{
    LinearVelocity, AngularVelocity
};
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
    Physics(Engine::Scene::Scene& scene, std::unique_ptr<IPhysicsAdapter> adapter);
    ~Physics();

    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    void Step(double deltaTime);
    void Reset();

    // Advances the selected adapter in fixed quanta, independently of output
    // frame sampling.
    void ConfigureFixedStep(double seconds, uint32_t maximumSubsteps,
        uint32_t solverIterations);
    double GetFixedStep() const;
    uint32_t GetMaximumSubsteps() const;
    uint32_t GetSolverIterations() const;
    uint32_t GetLastSubstepCount() const;

    // Runtime-only portal collision instances. They represent the remote
    // portion of a mesh while an object is split across a connection. The
    // remote piece participates in contact and ray queries; its owning
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

    // Returns every adapter hit in ascending distance order. Portal-aware
    // callers use this primitive for each straight chart segment.
    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin,
        const glm::vec3& direction, float maxDistance,
        uint32_t collisionMask = ~0u) const;

    void ApplyRigidBodyAction(Engine::Components::RigidBody& body,
        RigidBodyAction action, const glm::vec3& value,
        const glm::quat& rotation = {});
    glm::vec3 ReadRigidBodyVector(const Engine::Components::RigidBody& body,
        RigidBodyVector value) const;
    bool GetRigidBodyBounds(const Engine::Components::RigidBody& body,
        glm::vec3& minimum, glm::vec3& maximum) const;
    bool GetRigidBodyHullVertex(const Engine::Components::RigidBody& body,
        size_t index, glm::vec3& position) const;
    uint64_t GetRigidBodyGeneration(
        const Engine::Components::RigidBody& body) const;
    void NotifyRigidBodyTransformChanged(Engine::Components::RigidBody& body);
    void SetPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key, const std::vector<glm::vec3>& vertices);
    void ClearPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key);
    bool HasPortalLocalMeshCollider(
        const Engine::Components::RigidBody& body) const;
    void DestroyRigidBody(Engine::Components::RigidBody& body);
    bool IsClothSimulating(const Engine::Components::Cloth& cloth) const;
    void ResetCloth(Engine::Components::Cloth& cloth);
    void DestroyCloth(Engine::Components::Cloth& cloth);
    bool IsIKBoneSimulating(const Engine::Components::IKBone& bone) const;
    bool IsIKBoneUsingSkinnedCollider(
        const Engine::Components::IKBone& bone) const;
    bool GetIKBoneContactSeparation(const Engine::Components::IKBone& bone,
        const Engine::Components::RigidBody* other, float& separation) const;
    glm::vec3 ApplyIKBoneMeshContactTorque(Engine::Components::IKBone& bone,
        const glm::vec3& torqueImpulse, float limitPerMass);
    void ResetIKBone(Engine::Components::IKBone& bone);
    void DestroyIKBone(Engine::Components::IKBone& bone);

    IPhysicsAdapter& GetAdapter();
    const IPhysicsAdapter& GetAdapter() const;
    const char* GetAdapterName() const;

private:
    std::unique_ptr<IPhysicsAdapter> m_adapter;
};
}
