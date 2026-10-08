#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Scripts/Physics/FoxRagdollBlend.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <glm/gtc/quaternion.hpp>

namespace
{
float Angle(const glm::quat& a, const glm::quat& b)
{
    return 2.f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.f, 1.f));
}

bool RunFox(bool transformed)
{
    using namespace Engine::Components;
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_ragdoll_mesh.scene", nullptr))
        return false;
    auto* fox = scene.FindObjectByName("Fox");
    auto* ground = scene.FindObjectByName("Ragdoll Ground");
    auto* controllerObject = scene.FindObjectByName(
        "Fox Mesh Collider Blend Controller");
    auto* skeleton = fox ? fox->GetComponent<Skeleton>() : nullptr;
    auto* meshCollider = fox ? fox->GetComponent<MeshObjectCollider>() : nullptr;
    auto* body = fox ? fox->GetComponent<RigidBody>() : nullptr;
    auto* animation = fox ? fox->GetComponent<AnimationManager>() : nullptr;
    auto* controller = controllerObject
        ? controllerObject->GetComponent<FoxRagdollBlend>() : nullptr;
    auto* groundBody = ground ? ground->GetComponent<RigidBody>() : nullptr;
    auto* mesh = meshCollider
        ? Engine::Core::ResolveComponentReference<Mesh>(
            fox, meshCollider->meshReference) : nullptr;
    auto* skin = mesh && mesh->Owner
        ? mesh->Owner->GetComponent<SkinnedMesh>() : nullptr;
    if (!fox || !skeleton || !meshCollider || !body || !animation ||
        !controller || !groundBody || !mesh || !skin ||
        !controller->meshColliderDuringAnimation ||
        controller->meshColliderAfterBlend ||
        !controller->skinnedBoneHullsAfterBlend)
        return false;
    if (transformed)
    {
        fox->transform.rotation.y += 0.35f;
        fox->transform.scale *= 1.15f;
        fox->transform.MarkDirty();
    }
    scene.Start();
    if (!skeleton->UsesMeshCollider() || body->bodyType != "Kinematic")
        return false;
    const float startY = fox->transform.GetWorldPosition().y;
    std::vector<glm::quat> handoff;
    glm::vec3 lateRoot(0.f);
    bool mixedPhase = false;
    bool finishedPhase = false;
    float worstClearance = std::numeric_limits<float>::infinity();
    const int frames = transformed ? 540 : 1800;
    for (int frame = 0; frame < frames; ++frame)
    {
        scene.Update(1.f / 60.f);
        if (frame == 149)
        {
            unsigned active = 0;
            for (auto* bone : fox->GetComponentsInChildren<IKBone>())
                active += bone->IsSimulating() ? 1u : 0u;
            mixedPhase = skeleton->colliderMode == "SkinnedBoneHulls" &&
                animation->playing && active == 23 &&
                controller->GetInfluence() > 0.1f &&
                controller->GetInfluence() < 0.9f;
        }
        if (frame == 225)
        {
            finishedPhase = skeleton->colliderMode == "SkinnedBoneHulls" &&
                !animation->playing &&
                controller->GetInfluence() == 1.f;
            for (auto* bone : fox->GetComponentsInChildren<IKBone>())
                handoff.emplace_back(bone->Owner->transform.rotation);
        }
        if (frame == 1499)
            lateRoot = fox->transform.GetWorldPosition();
        if (frame < 300 || frame % 30 != 0) continue;
        glm::vec3 groundMinimum(0.f), groundMaximum(0.f);
        if (!groundBody->GetWorldCollisionBounds(
                groundMinimum, groundMaximum)) return false;
        const auto vertices = mesh->BuildPortalCutTriangleStream(
            &skin->BuildPalette());
        const glm::mat4 world = mesh->Owner->transform.GetWorldMatrix();
        for (const auto& vertex : vertices)
        {
            const glm::vec3 point(world * glm::vec4(vertex.pos[0],
                vertex.pos[1], vertex.pos[2], 1.f));
            if (point.x >= groundMinimum.x && point.x <= groundMaximum.x &&
                point.z >= groundMinimum.z && point.z <= groundMaximum.z)
                worstClearance = std::min(worstClearance,
                    point.y - groundMaximum.y);
        }
    }
    unsigned active = 0, fitted = 0, floorContacts = 0, bent = 0;
    float maximumBend = 0.f;
    const auto bones = fox->GetComponentsInChildren<IKBone>();
    for (size_t i = 0; i < bones.size(); ++i)
    {
        auto* bone = bones[i];
        if (!bone->IsSimulating()) continue;
        ++active;
        fitted += bone->IsUsingSkinnedCollider() ? 1u : 0u;
        float separation = 0.f;
        floorContacts += bone->GetContactSeparation(
            groundBody, separation) ? 1u : 0u;
        if (i < handoff.size())
        {
            const float bend = Angle(glm::quat(
                bone->Owner->transform.rotation), handoff[i]);
            maximumBend = std::max(maximumBend, bend);
            bent += bend > 0.1f ? 1u : 0u;
        }
    }
    const glm::vec3 endRoot = fox->transform.GetWorldPosition();
    std::fprintf(stderr,
        "Articulated mesh fox transformed=%d initialY=%.3f finalY=%.3f active=%u fitted=%u groundContacts=%u bent=%u maxBend=%.3f clearance=%.3f drift=%.3f\n",
        transformed, startY, endRoot.y, active, fitted,
        floorContacts, bent, maximumBend, worstClearance,
        transformed ? 0.f : glm::length(endRoot - lateRoot));
    return mixedPhase && finishedPhase && active == 23 && fitted == 22 &&
        floorContacts >= 3 && bent >= 18 && maximumBend > 0.9f &&
        endRoot.y < startY - 0.25f &&
        std::isfinite(worstClearance) && worstClearance >= -0.05f &&
        (transformed || glm::length(endRoot - lateRoot) < 0.1f);
}
}

int main()
{
    Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
    return RunFox(false) && RunFox(true) ? 0 : 1;
}
