#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Physics { class Physics; class BulletPhysicsAdapter; }
namespace Engine::Physics { struct PhysicsComponentState; }

namespace Engine::Components
{
// Simulates an independently selectable mesh as a soft body. The
// object's Mesh component is the rendered target and is restored on stop.
class Cloth final : public Engine::Core::Component
{
public:
    struct Anchor
    {
        glm::vec3 vertexPosition { 0.f };
        std::string bodyName;
        bool nearMode = false;
    };
    Cloth();
    ~Cloth() override;

    // Empty references preserve the conventional same-object Mesh defaults.
    // A different mesh can act as a lower-detail simulation cage, and the
    // rendered target may also live on another object.
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    ComponentReference simulationMeshReference { "Mesh" };
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    ComponentReference renderMeshReference { "Mesh" };
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    std::string meshPath;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    std::string topology = "TriangleMesh"; // TriangleMesh or Rope.
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", ClampMin = "0.001")
    float mass = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", Range = "0, 1")
    float linearStiffness = 0.8f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", Range = "0, 1")
    float bendingStiffness = 0.2f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", Range = "0, 1")
    float damping = 0.01f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", ClampMin = "0")
    float drag = 0.01f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", Range = "0, 1")
    float friction = 0.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    float gravityScale = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", ClampMin = "0.0001")
    float collisionMargin = 0.02f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth", ClampMin = "1")
    int solverIterations = 8;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Clusters", ClampMin = "0")
    int clusterCount = 0;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Clusters", ClampMin = "1")
    int clusterIterations = 4;
    glm::vec3 clusterHardness { .1f, 1.f, .5f };
    glm::vec3 clusterImpulseSplit { .5f };
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Filtering")
    int collisionLayer = 1;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Filtering")
    int collisionMask = -1;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    bool selfCollision = false;

    // Uses rigid-body contacts to squash and recover the rendered mesh without
    // creating a second, competing soft body on the same object.
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Collision Morph")
    bool collisionMorph = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Collision Morph", ClampMin = "0")
    float collisionMorphStrength = 0.075f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Collision Morph", Range = "0, 0.75")
    float collisionMorphMaximum = 0.34f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Collision Morph", ClampMin = "0")
    float collisionMorphRecovery = 4.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Collision Morph", Range = "0, 1")
    float collisionMorphExpansion = 0.55f;

    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Pinning")
    std::string pinMode = "Top"; // None, Top, Bottom, Left, Right
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Pinning", ClampMin = "0")
    float pinThreshold = 0.001f;
    // PMX soft-body pins and anchors use rest-space positions so duplicated
    // triangle vertices resolve to one welded Bullet node.
    std::vector<glm::vec3> explicitPinPositions;
    std::vector<Anchor> anchors;

    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Wind")
    glm::vec3 windVelocity { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Wind", ClampMin = "0")
    float windStrength = 0.f;

    bool IsSimulating() const;
    int GetActiveClusterCount() const;
    int GetActiveAnchorCount() const;
    int GetActiveLinkCount() const;
    int GetActiveFaceCount() const;
    void ResetSimulation();
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

    void Start() override;
    void Update() override;
    void Enabled() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Engine::Physics::Physics;
    friend class Engine::Physics::BulletPhysicsAdapter;
    bool EnsureSoftBody();
    bool EnsureCollisionMorph();
    void DestroySoftBody(bool restoreMesh);
    void ApplyForces();
    void UpdatePinnedNodes();
    void BindAnchors();
    void SyncMeshFromSoftBody();
    void NotifyRigidBodyCollision(const glm::vec3& worldNormal,
        const glm::vec3& worldPoint, float impulse);
    void UpdateCollisionMorph(float deltaTime);
    void NativeResetSimulation();
    bool NativeIsSimulating() const;

    struct Impl;
    static std::unique_ptr<Engine::Physics::PhysicsComponentState> MakeBulletState();
    void BindBulletState(Engine::Physics::PhysicsComponentState* state);
    Impl* m_impl = nullptr;
};
}
