#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace Engine::Physics { class Physics; class BulletPhysicsAdapter; }
namespace Engine::Physics { struct PhysicsComponentState; }

namespace Engine::Components
{
class RigidBody final : public Engine::Core::Component
{
public:
    struct FrictionBehavior
    {
        // All, Name, or Identifier. Specific matches take precedence over All.
        std::string match = "All";
        std::string target;
        float friction = 0.5f;
        bool enabled = true;
    };

    RigidBody();
    ~RigidBody() override;

    // Dynamic, Kinematic, or Static.
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    std::string bodyType = "Dynamic";
    PROPERTY(Inspector, EditAnywhere, Category = "Physics", ClampMin = "0.001")
    float mass = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    bool useGravity = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    float gravityScale = 1.f;
    // Persistent world-space direction for this body's local gravity frame.
    // Portal traversal remaps it so gravity remains coherent in the target
    // spatial chart. Magnitude is controlled independently by gravityScale.
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    glm::vec3 gravityDirection { 0.f, -1.f, 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Physics", ClampMin = "0")
    float linearDamping = 0.05f;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics", ClampMin = "0")
    float angularDamping = 0.05f;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics", Range = "0, 1")
    float friction = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics", Range = "0, 1")
    float restitution = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    bool isTrigger = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    bool continuousCollision = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    glm::vec3 initialLinearVelocity { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Physics")
    glm::vec3 initialAngularVelocity { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezePositionX = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezePositionY = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezePositionZ = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezeRotationX = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezeRotationY = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Constraints")
    bool freezeRotationZ = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Filtering")
    int collisionLayer = 1;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Filtering")
    int collisionMask = -1;
    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Filtering")
    std::string collisionIdentifier;

    PROPERTY(Inspector, EditAnywhere, Category = "Physics | Friction")
    std::vector<FrictionBehavior> frictionBehaviors;

    void AddForce(const glm::vec3& force);
    void AddTorque(const glm::vec3& torque);
    void AddImpulse(const glm::vec3& impulse);
    void AddAngularImpulse(const glm::vec3& impulse);
    void SetWorldPosition(const glm::vec3& worldPosition);
    void SetWorldPose(const glm::vec3& worldPosition, const glm::quat& worldRotation);
    void SetLinearVelocity(const glm::vec3& velocity);
    void SetAngularVelocity(const glm::vec3& velocity);
    void SetGravityDirection(const glm::vec3& direction);
    glm::vec3 GetLinearVelocity() const;
    glm::vec3 GetAngularVelocity() const;
    glm::vec3 GetGravityDirection() const;
    float ResolveFrictionFor(const RigidBody* other) const;
    bool IsOverlapping(const RigidBody* other) const;
    bool DidBeginOverlap(const RigidBody* other) const;
    bool DidEndOverlap(const RigidBody* other) const;
    std::vector<RigidBody*> GetOverlappingBodies() const;
    // Conservative bounds of the complete active collision shape.
    // Portal traversal samples this before replacing the shape with a split
    // half so aperture fit checks retain the original body footprint.
    bool GetWorldCollisionBounds(glm::vec3& minimum, glm::vec3& maximum) const;
    // Debugging and validation of skinned convex-hull alignment.
    bool GetWorldMeshHullVertex(size_t index, glm::vec3& position) const;
    uint64_t GetPhysicsBodyGeneration() const;
    bool IsColliding() const { return m_isColliding; }
    bool IsGrounded() const { return m_isGrounded; }
    void NotifyEditorTransformChanged();

    // Runtime-only local half of a portal-split mesh. This replaces the
    // owner's ordinary collider set while active so the dynamic body occupies
    // only its source-space portion; the remote portion is a Physics proxy.
    void SetPortalLocalMeshCollider(const void* instanceKey,
        const std::vector<glm::vec3>& localVertices);
    void ClearPortalLocalMeshCollider(const void* instanceKey);
    bool HasPortalLocalMeshCollider() const;

    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

    void Start() override;
    void Update() override;
    void Enabled() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Engine::Physics::Physics;
    friend class Engine::Physics::BulletPhysicsAdapter;
    void BeginOverlapFrame();
    void RegisterOverlap(const RigidBody* other);
    bool EnsureBody();
    void DestroyBody();
    void SyncBodyFromTransform();
    void SyncTransformFromBody();
    void ApplyBodySettings();
    void* GetNativeCollisionObjectForPhysics() const;
    void NativeAddForce(const glm::vec3& value);
    void NativeAddTorque(const glm::vec3& value);
    void NativeAddImpulse(const glm::vec3& value);
    void NativeAddAngularImpulse(const glm::vec3& value);
    void NativeSetWorldPosition(const glm::vec3& value);
    void NativeSetWorldPose(const glm::vec3& value, const glm::quat& rotation);
    void NativeSetLinearVelocity(const glm::vec3& value);
    void NativeSetAngularVelocity(const glm::vec3& value);
    void NativeSetGravityDirection(const glm::vec3& value);
    glm::vec3 NativeGetLinearVelocity() const;
    glm::vec3 NativeGetAngularVelocity() const;
    bool NativeGetWorldCollisionBounds(glm::vec3& minimum,
        glm::vec3& maximum) const;
    bool NativeGetWorldMeshHullVertex(size_t index, glm::vec3& position) const;
    uint64_t NativeGetPhysicsBodyGeneration() const;
    void NativeNotifyEditorTransformChanged();
    void NativeSetPortalLocalMeshCollider(const void* key,
        const std::vector<glm::vec3>& vertices);
    void NativeClearPortalLocalMeshCollider(const void* key);
    bool NativeHasPortalLocalMeshCollider() const;
    struct Impl;
    static std::unique_ptr<Engine::Physics::PhysicsComponentState> MakeBulletState();
    void BindBulletState(Engine::Physics::PhysicsComponentState* state);
    Impl* m_impl = nullptr;
    bool m_isColliding = false;
    bool m_isGrounded = false;
    bool m_editorTransformChanged = false;
    std::unordered_set<const RigidBody*> m_currentOverlaps;
    std::unordered_set<const RigidBody*> m_previousOverlaps;
};
}
