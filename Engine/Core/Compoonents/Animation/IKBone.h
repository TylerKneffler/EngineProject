#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

namespace Engine::Physics { class Physics; class BulletPhysicsAdapter; }
namespace Engine::Physics { struct PhysicsComponentState; }

namespace Engine::Components
{
class RigidBody;
class Skeleton;
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
    // Drive the skin pose in MeshCollider mode without using this segment for
    // collision contacts. The skeleton's mesh body owns world contacts.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone")
    bool poseOnlyWithMeshCollider = false;
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
    // Used while this body drives a mesh-collider skeleton after its root
    // becomes dynamic. Keeps the hidden joint chain from coasting forever.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body")
    float meshPoseLinearDamping = 0.8f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Body")
    float meshPoseAngularDamping = 1.f;
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
    // starts. Mesh-collider contact response also uses these values when this
    // bone has a Spring joint. Stiffness is torque per radian; damping is a
    // normalized ratio (zero is undamped, one is strongly damped).
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint | Spring", ClampMin = "0")
    float springStiffness = 20.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint | Spring", Range = "0, 1")
    float springDamping = 0.5f;
    // Secondary motion follows the animated pose while the rigid body is inactive.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Wiggle")
    bool wiggleEnabled = false;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Wiggle", ClampMin = "0")
    float wiggleStiffness = 35.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Wiggle", ClampMin = "0")
    float wiggleDamping = 8.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Wiggle", Range = "0, 3.14159")
    float wiggleMaxAngle = 0.45f;
    // Used if the bone has no child joint. Units match the authored skeleton.
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Wiggle", ClampMin = "0.001")
    float wiggleTipLength = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "IK Bone | Joint")
    bool collideWithParent = false;

    bool IsSimulating() const;
    bool IsUsingSkinnedCollider() const;
    bool GetContactSeparation(const RigidBody* other,
        float& deepestSeparation) const;
    void ResetSimulation();
    // Runtime pose blend supplied by the caller.
    void SetInfluence(float value);
    float GetInfluence() const { return m_influence; }
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Engine::Physics::Physics;
    friend class Engine::Physics::BulletPhysicsAdapter;
    friend class Skeleton;
    bool WantsSimulation() const;
    bool UsesMeshColliderPoseOnly() const;
    bool EnsureBody();
    bool EnsureConstraint();
    bool HasManualEditInHierarchy() const;
    void SyncBodyFromBone();
    void ApplyLiveBodySettings();
    void ApplyGroundedPoseGravity();
    void RemoveInvalidConstraint();
    void SyncBoneFromBody();
    void ApplyWiggle(float stepSeconds);
    void ResetWiggle();
    glm::vec3 ApplyMeshContactTorque(const glm::vec3& torqueImpulse,
        float limitPerMass);
    glm::vec3 NativeApplyMeshContactTorque(const glm::vec3& torqueImpulse,
        float limitPerMass);
    void DestroyConstraint(bool removeFromWorld = true);
    void DestroyBody(bool removeFromWorld = true);
    void NativeResetSimulation();
    bool NativeIsSimulating() const;
    bool NativeIsUsingSkinnedCollider() const;
    bool NativeGetContactSeparation(const RigidBody* other,
        float& deepestSeparation) const;
    IKBone* FindParentIKBone() const;
    Engine::Core::Object* FindSegmentChild(const std::string& childName) const;

    struct Impl;
    static std::unique_ptr<Engine::Physics::PhysicsComponentState> MakeBulletState();
    void BindBulletState(Engine::Physics::PhysicsComponentState* state);
    Impl* m_impl = nullptr;
    float m_influence = 1.f;
    glm::vec3 m_wiggleTip { 0.f };
    glm::vec3 m_wiggleVelocity { 0.f };
    glm::quat m_wiggleBaseRotation { 1.f, 0.f, 0.f, 0.f };
    glm::quat m_wiggleAppliedRotation { 1.f, 0.f, 0.f, 0.f };
    bool m_wiggleInitialized = false;
};
}
