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
    // Reference an enabled PrimitiveObjectCollider on this bone. Empty selects
    // the first enabled primitive collider on this bone.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Collider")
    ComponentReference colliderReference { "PrimitiveObjectCollider" };

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
    // Runtime pose blend supplied by the caller.
    void SetInfluence(float value);
    float GetInfluence() const { return m_influence; }
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Engine::Physics::Physics;
    bool WantsSimulation() const;
    bool EnsureBody();
    bool EnsureConstraint();
    bool HasManualEditInHierarchy() const;
    void SyncBodyFromBone();
    void RemoveInvalidConstraint();
    void SyncBoneFromBody();
    void DestroyConstraint(bool removeFromWorld = true);
    void DestroyBody(bool removeFromWorld = true);
    IKBone* FindParentIKBone() const;
    Engine::Core::Object* FindSegmentChild(const std::string& childName) const;

    struct Impl;
    Impl* m_impl = nullptr;
    float m_influence = 1.f;
};
}
