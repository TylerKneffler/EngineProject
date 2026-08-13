#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

class Physics;

// Simulates an independently selectable triangle mesh as a soft body. The
// object's Mesh component is the rendered target and is restored on stop.
class Cloth final : public Component
{
public:
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
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth")
    bool selfCollision = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Pinning")
    std::string pinMode = "Top"; // None, Top, Bottom, Left, Right
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Pinning", ClampMin = "0")
    float pinThreshold = 0.001f;

    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Wind")
    glm::vec3 windVelocity { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Cloth | Wind", ClampMin = "0")
    float windStrength = 0.f;

    bool IsSimulating() const;
    void ResetSimulation();

    void Start() override;
    void Update() override;
    void Enabled() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    friend class Physics;
    bool EnsureSoftBody();
    void DestroySoftBody(bool restoreMesh);
    void ApplyForces();
    void UpdatePinnedNodes();
    void SyncMeshFromSoftBody();

    struct Impl;
    Impl* m_impl = nullptr;
};
