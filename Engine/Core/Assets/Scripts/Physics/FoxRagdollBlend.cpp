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
}

bool FoxRagdollBlend::ConfigureFox()
{
    auto* fox = Owner ? Owner->FindObjectInSceneByName(foxObjectName) : nullptr;
    auto* skeleton = fox ? fox->GetComponent<Engine::Components::Skeleton>()
        : nullptr;
    if (!skeleton) return false;

    skeleton->colliderMode = meshColliderDuringAnimation || meshColliderAfterBlend
        ? "MeshCollider" : "PerBone";
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
            skeleton->colliderMode = "PerBone";
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
            for (auto* bone : fox->GetComponentsInChildren<Engine::Components::IKBone>())
                bone->simulate = false;
            if (auto* body = fox->GetComponent<Engine::Components::RigidBody>())
            {
                body->bodyType = "Dynamic";
                body->useGravity = true;
                body->freezeRotationX = true;
                body->freezeRotationY = true;
                body->freezeRotationZ = true;
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
            : meshColliderAfterBlend ? "Mesh-collider physics" : "IK physics";
    char influence[32];
    std::snprintf(influence, sizeof(influence), "%.0f%%", m_influence * 100.f);
    ui.ValueLabel("Phase", phase);
    ui.ValueLabel("IK Influence", influence);
    return changed;
}
