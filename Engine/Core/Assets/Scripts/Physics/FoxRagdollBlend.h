#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

// Runs animation, then blends into IK. Mesh-collider scenes keep the bones
// active to deform the skin while the dynamic mesh body handles world contact.
class FoxRagdollBlend final : public Engine::Core::Script
{
public:
    FoxRagdollBlend();

    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    std::string foxObjectName = "Fox";
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float animationSeconds = 1.25f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float blendSeconds = 2.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    bool meshColliderDuringAnimation = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    bool meshColliderAfterBlend = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll")
    bool skinnedBoneHullsAfterBlend = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshContactStrength = 0.6f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshContactMaxBend = 0.15f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshRagdollMaxBend = 0.3f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshRagdollGravityStrength = 1.2f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", Range = "0, 1")
    float meshRagdollBoneAngularDamping = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", Range = "0, 1")
    float meshRagdollBoneGravityScale = 0.02f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshRagdollContactTorqueLimit = 0.015f;
    PROPERTY(Inspector, EditAnywhere, Category = "Fox Ragdoll", ClampMin = "0")
    float meshRagdollImpactThreshold = 0.5f;

    void Start() override;
    void Update() override;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;
    float GetInfluence() const { return m_influence; }

private:
    bool ConfigureFox();
    float m_elapsed = 0.f;
    float m_influence = 0.f;
    bool m_configured = false;
    bool m_blending = false;
    bool m_finished = false;
};
