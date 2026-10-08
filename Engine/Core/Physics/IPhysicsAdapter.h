#pragma once

#include "Core/Physics/Physics.h"

namespace Engine::Physics
{
struct PhysicsComponentState
{
    virtual ~PhysicsComponentState() = default;
};

// One adapter coordinates simulation and component operations for a scene.
// Engine code and scene scripts use Physics as their connection.
class IPhysicsAdapter
{
public:
    virtual ~IPhysicsAdapter() = default;
    virtual const char* Name() const = 0;
    virtual void Step(double deltaTime) = 0;
    virtual void Reset() = 0;
    virtual void ConfigureFixedStep(double seconds, uint32_t maximumSubsteps,
        uint32_t solverIterations) = 0;
    virtual double GetFixedStep() const = 0;
    virtual uint32_t GetMaximumSubsteps() const = 0;
    virtual uint32_t GetSolverIterations() const = 0;
    virtual uint32_t GetLastSubstepCount() const = 0;
    virtual void SetPortalMeshCollider(const void* instanceKey,
        const Engine::Components::RigidBody& owner,
        const std::vector<glm::vec3>& worldVertices) = 0;
    virtual void RemovePortalMeshCollider(const void* instanceKey) = 0;
    virtual size_t GetPortalMeshColliderCount(
        const Engine::Components::RigidBody& owner) const = 0;
    virtual void SetPortalApertureCollider(const void* instanceKey,
        const std::vector<glm::vec3>& worldPoints,
        const glm::vec3& worldNormal, float edgeHalfWidth,
        float edgeHalfDepth) = 0;
    virtual void RemovePortalApertureCollider(const void* instanceKey) = 0;
    virtual std::vector<Physics::RaycastHit> RaycastAll(
        const glm::vec3& origin, const glm::vec3& direction,
        float maxDistance, uint32_t collisionMask) const = 0;

    // Component operations use engine values only. The Bullet implementation
    // dispatches these to its native component state; another adapter can
    // supply its own component state behind the same methods.
    virtual void ApplyRigidBodyAction(Engine::Components::RigidBody& body,
        RigidBodyAction action, const glm::vec3& value,
        const glm::quat& rotation) = 0;
    virtual glm::vec3 ReadRigidBodyVector(
        const Engine::Components::RigidBody& body,
        RigidBodyVector value) const = 0;
    virtual bool GetRigidBodyBounds(const Engine::Components::RigidBody& body,
        glm::vec3& minimum, glm::vec3& maximum) const = 0;
    virtual bool GetRigidBodyHullVertex(const Engine::Components::RigidBody& body,
        size_t index, glm::vec3& position) const = 0;
    virtual uint64_t GetRigidBodyGeneration(
        const Engine::Components::RigidBody& body) const = 0;
    virtual void NotifyRigidBodyTransformChanged(
        Engine::Components::RigidBody& body) = 0;
    virtual void SetPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key, const std::vector<glm::vec3>& vertices) = 0;
    virtual void ClearPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key) = 0;
    virtual bool HasPortalLocalMeshCollider(
        const Engine::Components::RigidBody& body) const = 0;
    virtual void DestroyRigidBody(Engine::Components::RigidBody& body) = 0;
    virtual bool IsClothSimulating(const Engine::Components::Cloth& cloth) const = 0;
    virtual void ResetCloth(Engine::Components::Cloth& cloth) = 0;
    virtual void DestroyCloth(Engine::Components::Cloth& cloth) = 0;
    virtual bool IsIKBoneSimulating(const Engine::Components::IKBone& bone) const = 0;
    virtual bool IsIKBoneUsingSkinnedCollider(
        const Engine::Components::IKBone& bone) const = 0;
    virtual bool GetIKBoneContactSeparation(
        const Engine::Components::IKBone& bone,
        const Engine::Components::RigidBody* other,
        float& separation) const = 0;
    virtual glm::vec3 ApplyIKBoneMeshContactTorque(
        Engine::Components::IKBone& bone,
        const glm::vec3& torqueImpulse, float limitPerMass) = 0;
    virtual void ResetIKBone(Engine::Components::IKBone& bone) = 0;
    virtual void DestroyIKBone(Engine::Components::IKBone& bone) = 0;

};

std::unique_ptr<IPhysicsAdapter> CreateBulletPhysicsAdapter(
    Engine::Scene::Scene& scene);
}
