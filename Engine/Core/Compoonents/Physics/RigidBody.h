#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

class PhysicsSystem;

class RigidBody final : public Script
{
public:
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

    void AddForce(const glm::vec3& force);
    void AddTorque(const glm::vec3& torque);
    void AddImpulse(const glm::vec3& impulse);
    void SetLinearVelocity(const glm::vec3& velocity);
    void SetAngularVelocity(const glm::vec3& velocity);
    glm::vec3 GetLinearVelocity() const;
    glm::vec3 GetAngularVelocity() const;
    bool IsColliding() const { return m_isColliding; }
    bool IsGrounded() const { return m_isGrounded; }

    void Start() override;
    void Update() override;
    void Enabled() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class PhysicsSystem;
    bool EnsureBody();
    void DestroyBody();
    void SyncBodyFromTransform();
    void SyncTransformFromBody();
    void ApplyBodySettings();
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_isColliding = false;
    bool m_isGrounded = false;
};
