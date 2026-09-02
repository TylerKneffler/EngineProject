#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace Engine::Physics { class Physics; }

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
    void SetWorldPosition(const glm::vec3& worldPosition);
    void SetWorldPose(const glm::vec3& worldPosition, const glm::quat& worldRotation);
    void SetLinearVelocity(const glm::vec3& velocity);
    void SetAngularVelocity(const glm::vec3& velocity);
    glm::vec3 GetLinearVelocity() const;
    glm::vec3 GetAngularVelocity() const;
    float ResolveFrictionFor(const RigidBody* other) const;
    bool IsOverlapping(const RigidBody* other) const;
    bool DidBeginOverlap(const RigidBody* other) const;
    bool DidEndOverlap(const RigidBody* other) const;
    std::vector<RigidBody*> GetOverlappingBodies() const;
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
    void BeginOverlapFrame();
    void RegisterOverlap(const RigidBody* other);
    bool EnsureBody();
    void DestroyBody();
    void SyncBodyFromTransform();
    void SyncTransformFromBody();
    void ApplyBodySettings();
    void* GetNativeCollisionObjectForPhysics() const;
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_isColliding = false;
    bool m_isGrounded = false;
    bool m_editorTransformChanged = false;
};
}
