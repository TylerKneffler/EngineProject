#include "Scripts/Physics/FoxRagdollBlend.h"

#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cstdio>

namespace
{
struct FoxRagdollBlendRegistration
{
    FoxRagdollBlendRegistration()
    {
        Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
            "FoxRagdollBlend");
    }
};
FoxRagdollBlendRegistration g_registration;
}

FoxRagdollBlend::FoxRagdollBlend()
{
    SetTypeName(COMPONENT_TYPE_NAME(FoxRagdollBlend));
    RegisterField("foxObjectName", foxObjectName, "Fox Ragdoll");
    RegisterField("animationSeconds", animationSeconds, "Fox Ragdoll");
    RegisterField("blendSeconds", blendSeconds, "Fox Ragdoll");
    RegisterField("meshColliderDuringAnimation", meshColliderDuringAnimation,
        "Fox Ragdoll");
    RegisterField("meshColliderAfterBlend", meshColliderAfterBlend, "Fox Ragdoll");
    RegisterField("skinnedBoneHullsAfterBlend", skinnedBoneHullsAfterBlend,
        "Fox Ragdoll");
    RegisterField("meshContactStrength", meshContactStrength, "Fox Ragdoll");
    RegisterField("meshContactMaxBend", meshContactMaxBend, "Fox Ragdoll");
    RegisterField("meshRagdollMaxBend", meshRagdollMaxBend, "Fox Ragdoll");
    RegisterField("meshRagdollGravityStrength", meshRagdollGravityStrength,
        "Fox Ragdoll");
    RegisterField("meshRagdollBoneAngularDamping",
        meshRagdollBoneAngularDamping, "Fox Ragdoll");
    RegisterField("meshRagdollBoneGravityScale",
        meshRagdollBoneGravityScale, "Fox Ragdoll");
    RegisterField("meshRagdollContactTorqueLimit",
        meshRagdollContactTorqueLimit, "Fox Ragdoll");
    RegisterField("meshRagdollImpactThreshold",
        meshRagdollImpactThreshold, "Fox Ragdoll");
}

bool FoxRagdollBlend::ConfigureFox()
{
    auto* fox = Owner ? Owner->FindObjectInSceneByName(foxObjectName) : nullptr;
    auto* skeleton = fox ? fox->GetComponent<Engine::Components::Skeleton>()
        : nullptr;
    if (!skeleton) return false;

    skeleton->colliderMode = meshColliderDuringAnimation || meshColliderAfterBlend
        ? "MeshCollider" : "PerBone";
    if (meshColliderAfterBlend)
    {
        skeleton->meshRagdollEnabled = false;
        skeleton->meshContactResponseStrength =
            std::max(meshContactStrength, 0.f);
        skeleton->meshContactMaxBend =
            std::max(meshContactMaxBend, 0.f);
    }
    skeleton->MarkConfigurationDirty();
    auto* body = fox->GetComponent<Engine::Components::RigidBody>();
    if (!body) return false;
    body->bodyType = "Kinematic";
    body->useGravity = false;
    body->freezeRotationX = false;
    body->freezeRotationY = false;
    body->freezeRotationZ = false;
    body->MarkConfigurationDirty();
    if (auto* animation = fox->GetComponent<Engine::Components::AnimationManager>())
    {
        animation->playing = true;
        animation->holdCurrentPoseWhenStopped = false;
    }
    for (auto* bone : fox->GetComponentsInChildren<Engine::Components::IKBone>())
    {
        bone->simulate = false;
        bone->poseOnlyWithMeshCollider = meshColliderAfterBlend;
        bone->SetInfluence(0.f);
        bone->ResetSimulation();
    }
    return true;
}

void FoxRagdollBlend::Start()
{
    m_elapsed = 0.f;
    m_influence = 0.f;
    m_blending = false;
    m_finished = false;
    m_configured = ConfigureFox();
}

void FoxRagdollBlend::Update()
{
    if (!Owner || !Owner->GetScene()) return;
    if (!m_configured)
        m_configured = ConfigureFox();
    if (!m_configured) return;
    if (m_finished) return;

    m_elapsed += std::max(Owner->GetScene()->GetDeltaTime(), 0.f);
    const float hold = std::max(animationSeconds, 0.f);
    const float duration = std::max(blendSeconds, 0.f);
    const bool physicsStarted = m_elapsed >= hold;
    m_influence = physicsStarted
        ? duration <= 0.f ? 1.f
            : std::clamp((m_elapsed - hold) / duration, 0.f, 1.f)
        : 0.f;

    auto* fox = Owner->FindObjectInSceneByName(foxObjectName);
    if (!fox) return;
    auto* skeleton = fox->GetComponent<Engine::Components::Skeleton>();
    if (physicsStarted && !m_blending && skeleton)
    {
        if (!meshColliderAfterBlend)
        {
            skeleton->colliderMode = skinnedBoneHullsAfterBlend
                ? "SkinnedBoneHulls" : "PerBone";
            skeleton->MarkConfigurationDirty();
        }
        m_blending = true;
    }
    for (auto* bone : fox->GetComponentsInChildren<Engine::Components::IKBone>())
    {
        bone->simulate = physicsStarted;
        bone->SetInfluence(m_influence);
    }
    if (m_influence >= 1.f)
    {
        if (auto* animation = fox->GetComponent<Engine::Components::AnimationManager>())
        {
            animation->holdCurrentPoseWhenStopped = meshColliderAfterBlend;
            animation->playing = false;
        }
        if (meshColliderAfterBlend)
        {
            if (skeleton)
            {
                skeleton->meshRagdollEnabled = true;
                skeleton->meshRagdollGravityStrength =
                    std::max(meshRagdollGravityStrength, 0.f);
                skeleton->meshContactMaxBend =
                    std::max(meshRagdollMaxBend, 0.f);
                skeleton->meshIKGroundedGravityScale =
                    std::clamp(meshRagdollBoneGravityScale, 0.f, 1.f);
                skeleton->meshIKTorqueLimitPerMass =
                    std::max(meshRagdollContactTorqueLimit, 0.f);
                skeleton->meshImpactImpulseThreshold =
                    std::max(meshRagdollImpactThreshold, 0.f);
            }
            for (auto* bone : fox->GetComponentsInChildren<Engine::Components::IKBone>())
                bone->meshPoseAngularDamping =
                    std::clamp(meshRagdollBoneAngularDamping, 0.f, 1.f);
            if (auto* body = fox->GetComponent<Engine::Components::RigidBody>())
            {
                body->bodyType = "Dynamic";
                body->useGravity = true;
                body->freezeRotationX = false;
                body->freezeRotationY = false;
                body->freezeRotationZ = false;
                body->angularDamping = 0.9f;
                body->continuousCollision = true;
                body->MarkConfigurationDirty();
            }
        }
        m_finished = true;
    }
}

bool FoxRagdollBlend::DrawProperties(Engine::Editor::IEditorUi& ui)
{
    const bool changed = Engine::Core::Component::DrawProperties(ui);
    const char* phase = m_elapsed < std::max(animationSeconds, 0.f)
        ? "Animation" : m_influence < 1.f ? "Animation + IK"
            : meshColliderAfterBlend ? "Mesh pose physics"
                : skinnedBoneHullsAfterBlend ? "Skinned mesh ragdoll"
                    : "IK physics";
    char influence[32];
    std::snprintf(influence, sizeof(influence), "%.0f%%", m_influence * 100.f);
    ui.ValueLabel("Phase", phase);
    ui.ValueLabel("Bone Blend", influence);
    return changed;
}
