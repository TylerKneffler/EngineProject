#pragma once

#include "Core/Physics/IPhysicsAdapter.h"
#include <unordered_map>

namespace Engine::Physics
{
class PhysicsWorldState;
PhysicsWorldState& StateFor(Engine::Scene::Scene* scene);
class BulletPhysicsAdapter final : public IPhysicsAdapter
{
public:
    using RaycastHit = Physics::RaycastHit;
    using RaycastHitKind = Physics::RaycastHitKind;

    explicit BulletPhysicsAdapter(Engine::Scene::Scene& scene);
    ~BulletPhysicsAdapter() override;
    const char* Name() const override { return "Bullet"; }
    void Step(double deltaTime) override;
    void Reset() override;
    void ConfigureFixedStep(double seconds, uint32_t maximumSubsteps,
        uint32_t solverIterations) override;
    double GetFixedStep() const override;
    uint32_t GetMaximumSubsteps() const override;
    uint32_t GetSolverIterations() const override;
    uint32_t GetLastSubstepCount() const override;
    void SetPortalMeshCollider(const void* instanceKey,
        const Engine::Components::RigidBody& owner,
        const std::vector<glm::vec3>& worldVertices) override;
    void RemovePortalMeshCollider(const void* instanceKey) override;
    size_t GetPortalMeshColliderCount(
        const Engine::Components::RigidBody& owner) const override;
    void SetPortalApertureCollider(const void* instanceKey,
        const std::vector<glm::vec3>& worldPoints,
        const glm::vec3& worldNormal, float edgeHalfWidth,
        float edgeHalfDepth) override;
    void RemovePortalApertureCollider(const void* instanceKey) override;
    std::vector<RaycastHit> RaycastAll(const glm::vec3& origin,
        const glm::vec3& direction, float maxDistance,
        uint32_t collisionMask) const override;
    void ApplyRigidBodyAction(Engine::Components::RigidBody& body,
        RigidBodyAction action, const glm::vec3& value,
        const glm::quat& rotation) override;
    glm::vec3 ReadRigidBodyVector(
        const Engine::Components::RigidBody& body,
        RigidBodyVector value) const override;
    bool GetRigidBodyBounds(const Engine::Components::RigidBody& body,
        glm::vec3& minimum, glm::vec3& maximum) const override;
    bool GetRigidBodyHullVertex(const Engine::Components::RigidBody& body,
        size_t index, glm::vec3& position) const override;
    uint64_t GetRigidBodyGeneration(
        const Engine::Components::RigidBody& body) const override;
    void NotifyRigidBodyTransformChanged(
        Engine::Components::RigidBody& body) override;
    void SetPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key, const std::vector<glm::vec3>& vertices) override;
    void ClearPortalLocalMeshCollider(Engine::Components::RigidBody& body,
        const void* key) override;
    bool HasPortalLocalMeshCollider(
        const Engine::Components::RigidBody& body) const override;
    void DestroyRigidBody(Engine::Components::RigidBody& body) override;
    bool IsClothSimulating(const Engine::Components::Cloth& cloth) const override;
    void ResetCloth(Engine::Components::Cloth& cloth) override;
    void DestroyCloth(Engine::Components::Cloth& cloth) override;
    bool IsIKBoneSimulating(const Engine::Components::IKBone& bone) const override;
    bool IsIKBoneUsingSkinnedCollider(
        const Engine::Components::IKBone& bone) const override;
    bool GetIKBoneContactSeparation(const Engine::Components::IKBone& bone,
        const Engine::Components::RigidBody* other,
        float& separation) const override;
    glm::vec3 ApplyIKBoneMeshContactTorque(Engine::Components::IKBone& bone,
        const glm::vec3& torqueImpulse, float limitPerMass) override;
    void ResetIKBone(Engine::Components::IKBone& bone) override;
    void DestroyIKBone(Engine::Components::IKBone& bone) override;

private:
    friend PhysicsWorldState& StateFor(Engine::Scene::Scene* scene);
    void EnsureRigidBodyState(Engine::Components::RigidBody& body);
    void EnsureClothState(Engine::Components::Cloth& cloth);
    void EnsureIKBoneState(Engine::Components::IKBone& bone);
    PhysicsWorldState* GetInternalState();
    struct Impl;
    Impl* m_impl = nullptr;
    std::unordered_map<const void*, std::unique_ptr<PhysicsComponentState>>
        m_componentStates;
};
}
