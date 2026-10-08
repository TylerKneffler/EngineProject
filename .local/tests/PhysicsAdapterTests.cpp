#include "Core/Physics/IPhysicsAdapter.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include <cmath>
#include <cstdio>

namespace
{
class RecordingAdapter final : public Engine::Physics::IPhysicsAdapter
{
public:
    using RigidBody = Engine::Components::RigidBody;
    using Cloth = Engine::Components::Cloth;
    using IKBone = Engine::Components::IKBone;
    using RaycastHit = Engine::Physics::Physics::RaycastHit;
    const char* Name() const override { return "Recording"; }
    void Step(double) override { ++steps; }
    void Reset() override { ++resets; }
    void ConfigureFixedStep(double, uint32_t, uint32_t) override {}
    double GetFixedStep() const override { return 1.0 / 60.0; }
    uint32_t GetMaximumSubsteps() const override { return 1; }
    uint32_t GetSolverIterations() const override { return 1; }
    uint32_t GetLastSubstepCount() const override { return 0; }
    void SetPortalMeshCollider(const void*, const RigidBody&,
        const std::vector<glm::vec3>&) override {}
    void RemovePortalMeshCollider(const void*) override {}
    size_t GetPortalMeshColliderCount(const RigidBody&) const override { return 0; }
    void SetPortalApertureCollider(const void*, const std::vector<glm::vec3>&,
        const glm::vec3&, float, float) override {}
    void RemovePortalApertureCollider(const void*) override {}
    std::vector<RaycastHit> RaycastAll(const glm::vec3&,
        const glm::vec3&, float, uint32_t) const override
    {
        RaycastHit hit;
        hit.distance = 2.f;
        return { hit };
    }
    void ApplyRigidBodyAction(RigidBody&, Engine::Physics::RigidBodyAction action,
        const glm::vec3& value, const glm::quat&) override
    {
        lastAction = action;
        lastValue = value;
        ++bodyActions;
    }
    glm::vec3 ReadRigidBodyVector(const RigidBody&,
        Engine::Physics::RigidBodyVector) const override
    { return glm::vec3(3.f, 4.f, 5.f); }
    bool GetRigidBodyBounds(const RigidBody&, glm::vec3&,
        glm::vec3&) const override { return false; }
    bool GetRigidBodyHullVertex(const RigidBody&, size_t,
        glm::vec3&) const override { return false; }
    uint64_t GetRigidBodyGeneration(const RigidBody&) const override { return 17; }
    void NotifyRigidBodyTransformChanged(RigidBody&) override {}
    void SetPortalLocalMeshCollider(RigidBody&, const void*,
        const std::vector<glm::vec3>&) override {}
    void ClearPortalLocalMeshCollider(RigidBody&, const void*) override {}
    bool HasPortalLocalMeshCollider(const RigidBody&) const override { return false; }
    void DestroyRigidBody(RigidBody&) override { ++bodyDestroys; }
    bool IsClothSimulating(const Cloth&) const override { return true; }
    void ResetCloth(Cloth&) override { ++clothResets; }
    void DestroyCloth(Cloth&) override { ++clothDestroys; }
    bool IsIKBoneSimulating(const IKBone&) const override { return true; }
    bool IsIKBoneUsingSkinnedCollider(const IKBone&) const override
    { return true; }
    bool GetIKBoneContactSeparation(const IKBone&, const RigidBody*,
        float& separation) const override
    { separation = -0.25f; return true; }
    glm::vec3 ApplyIKBoneMeshContactTorque(IKBone&,
        const glm::vec3& torque, float) override { return torque; }
    void ResetIKBone(IKBone&) override { ++boneResets; }
    void DestroyIKBone(IKBone&) override { ++boneDestroys; }

    Engine::Physics::RigidBodyAction lastAction =
        Engine::Physics::RigidBodyAction::AddForce;
    glm::vec3 lastValue { 0.f };
    int steps = 0, resets = 0, bodyActions = 0;
    int clothResets = 0, boneResets = 0;
    int bodyDestroys = 0, clothDestroys = 0, boneDestroys = 0;
};
}

int main()
{
    using Engine::Physics::RigidBodyAction;
    RecordingAdapter* adapter = nullptr;
    {
        Engine::Scene::Scene scene([&adapter](Engine::Scene::Scene&)
        {
            auto created = std::make_unique<RecordingAdapter>();
            adapter = created.get();
            return created;
        });
        auto* object = scene.AddObject("Adapter test");
        auto* body = object->AddComponent<Engine::Components::RigidBody>();
        auto* cloth = object->AddComponent<Engine::Components::Cloth>();
        auto* bone = object->AddComponent<Engine::Components::IKBone>();
        body->AddImpulse(glm::vec3(1.f, 2.f, 3.f));
        cloth->ResetSimulation();
        bone->ResetSimulation();
        scene.GetPhysics().Step(1.0 / 60.0);
        const auto hits = scene.GetPhysics().RaycastAll(
            glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f), 10.f);
        float separation = 0.f;
        if (adapter->bodyActions != 1 ||
            adapter->lastAction != RigidBodyAction::AddImpulse ||
            adapter->lastValue != glm::vec3(1.f, 2.f, 3.f) ||
            adapter->clothResets != 1 || adapter->boneResets != 1 ||
            adapter->steps != 1 || hits.size() != 1 ||
            hits[0].distance != 2.f ||
            body->GetLinearVelocity() != glm::vec3(3.f, 4.f, 5.f) ||
            body->GetPhysicsBodyGeneration() != 17 ||
            !cloth->IsSimulating() || !bone->IsSimulating() ||
            !bone->GetContactSeparation(body, separation) ||
            std::abs(separation + 0.25f) > 1e-5f)
        {
            std::fputs("Physics adapter dispatch failed\n", stderr);
            return 1;
        }
        body->Disabled();
        cloth->Disabled();
        bone->Disabled();
        if (adapter->bodyDestroys != 1 || adapter->clothDestroys != 1 ||
            adapter->boneDestroys != 1)
            return 2;
    }
    {
        // Clearing a scene must release adapter-owned state before object
        // addresses can be reused by the next set of components.
        Engine::Scene::Scene scene;
        for (int iteration = 0; iteration < 2; ++iteration)
        {
            auto* object = scene.AddObject("Reset lifetime test");
            auto* body = object->AddComponent<Engine::Components::RigidBody>();
            body->bodyType = "Static";
            object->AddComponent<Engine::Components::PrimitiveObjectCollider>();
            scene.GetPhysics().Step(1.0 / 60.0);
            scene.ClearObjects();
        }
    }
    return 0;
}
