#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

namespace Engine::Physics { class Physics; }

namespace Engine::Components
{
// One independently authored rigid segment of an animated skeleton. Attach it
// directly to an AnimationBone object. A collection of IKBone components forms a
// ragdoll; no separate ragdoll controller or generated bone list is required.
class IKBone final : public Engine::Core::Component
{
public:
    IKBone();
    ~IKBone() override;

    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone")
    bool simulate = true;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone", ClampMin = "0")
    float activationDelay = 0.f;
    // Zero leaves the animation pose unchanged. One fully replaces this
    // bone's local pose with the simulated pose; values between blend them.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone", Range = "0, 1")
    float weight = 1.f;
    // Capsule follows this direct child bone. Empty selects the first child
    // that also has an IKBone component.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Collider")
    std::string childBone;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Collider")
    std::string shape = "Capsule"; // Capsule or Sphere
    // Zero capsule length derives it from this bone to childBone.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Collider", ClampMin = "0")
    float length = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Collider", ClampMin = "0.001")
    float radius = 0.1f;

    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body", ClampMin = "0.001")
    float mass = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body", Range = "0, 1")
    float friction = 0.65f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body", Range = "0, 1")
    float linearDamping = 0.08f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body", Range = "0, 1")
    float angularDamping = 0.18f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body")
    glm::vec3 initialLinearVelocity { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body")
    glm::vec3 initialAngularVelocity { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint")
    bool connectToParent = true;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint")
    std::string jointType = "ConeTwist"; // ConeTwist, Hinge, Spring, or Fixed
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint", Range = "0, 3.14159")
    float swingLimit = 0.8f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint", Range = "0, 3.14159")
    float twistLimit = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint", Range = "0, 3.14159")
    float hingeLimit = 1.2f;
    // Spring joints return toward the authored pose captured when simulation
    // starts. Stiffness is torque per radian; damping is Bullet's normalized
    // damping ratio (zero is undamped, one is strongly damped).
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint | Spring", ClampMin = "0")
    float springStiffness = 20.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint | Spring", Range = "0, 1")
    float springDamping = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint")
    bool collideWithParent = false;

    bool IsSimulating() const;
    void ResetSimulation();
    void Start() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Engine::Physics::Physics;
    void AdvanceActivation(float deltaTime);
    bool WantsSimulation() const;
    bool EnsureBody();
    bool EnsureConstraint();
    void RemoveInvalidConstraint();
    void SyncBoneFromBody();
    void DestroyConstraint(bool removeFromWorld = true);
    void DestroyBody(bool removeFromWorld = true);
    IKBone* FindParentIKBone() const;
    Engine::Core::Object* FindSegmentChild() const;

    struct Impl;
    Impl* m_impl = nullptr;
    float m_elapsed = 0.f;
};
}
