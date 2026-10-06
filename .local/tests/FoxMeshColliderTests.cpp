#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>

int main()
{
    using namespace Engine::Components;
    Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_mesh_collider.scene", nullptr))
        return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* skeleton = fox ? fox->GetComponent<Skeleton>() : nullptr;
    auto* collider = fox ? fox->GetComponent<MeshObjectCollider>() : nullptr;
    auto* body = fox ? fox->GetComponent<RigidBody>() : nullptr;
    auto* skinnedMesh = collider
        ? Engine::Core::ResolveComponentReference<Mesh>(
            fox, collider->meshReference) : nullptr;
    auto* animation = fox ? fox->GetComponent<AnimationManager>() : nullptr;
    auto* controllerObject = scene.FindObjectByName(
        "Fox Mesh Collider Blend Controller");
    auto* controller = controllerObject
        ? controllerObject->GetComponent<FoxRagdollBlend>() : nullptr;
    if (!skeleton || !skeleton->UsesMeshCollider() ||
        !collider || skeleton->ResolveMeshCollider() != collider ||
        !body || !skinnedMesh || !animation || !controller ||
        body->bodyType != "Kinematic" ||
        !collider->meshReference.IsAssigned())
        return 2;
    auto* probe = scene.FindObjectByName("Fox Mesh Contact Probe");
    auto* probeCollider = probe
        ? probe->GetComponent<PrimitiveObjectCollider>() : nullptr;
    auto* probeBody = probe ? probe->GetComponent<RigidBody>() : nullptr;
    if (!probeCollider || !probeBody || probeBody->bodyType != "Dynamic")
        return 2;
    auto* ground = scene.FindObjectByName("Ragdoll Ground");
    auto* groundBody = ground ? ground->GetComponent<RigidBody>() : nullptr;
    if (!groundBody) return 2;
    scene.Start();
    glm::vec3 minimum(0.f), maximum(0.f);
    glm::vec3 initialMinimum(0.f), initialMaximum(0.f);
    unsigned activeBoneBodies = 0;
    bool touchedFox = false;
    bool sawMeshContact = false;
    bool sawMappedContact = false;
    bool sawWeightedContact = false;
    bool sawResolvedBone = false;
    bool sawPostHandoffBoneMotion = false;
    bool sawPostHandoffWeightedContact = false;
    std::vector<glm::vec3> handoffBoneRotations;
    bool sawGroundContact = false;
    bool poseChanged = false;
    bool initialMeshCollider = false;
    bool blendedMeshCollider = false;
    bool poseBonesActiveDuringBlend = false;
    bool meshCollisionActiveEveryFrame = true;
    glm::vec3 poseBeforeHandoff(0.f), poseAfterHandoff(0.f);
    bool sampledBeforeHandoff = false, sampledAfterHandoff = false;
    const float startHeight = fox->transform.GetWorldPosition().y;
    glm::vec3 lateRoot(0.f), finalRoot(0.f);
    std::vector<glm::vec3> lateBoneRotations, finalBoneRotations;
    auto* skin = skinnedMesh->Owner->GetComponent<SkinnedMesh>();
    if (!skin) return 4;
    for (int frame = 0; frame < 1800; ++frame)
    {
        scene.Update(1.f / 60.f);
        if (frame == 1499 || frame == 1799)
        {
            auto& rotations = frame == 1499
                ? lateBoneRotations : finalBoneRotations;
            for (AnimationBone* bone : skeleton->ResolveBones())
                rotations.push_back(bone && bone->Owner
                    ? bone->Owner->transform.rotation : glm::vec3(0.f));
            if (frame == 1499) lateRoot = fox->transform.GetWorldPosition();
            else finalRoot = fox->transform.GetWorldPosition();
        }
        touchedFox = touchedFox || body->IsColliding();
        for (const auto& contact : collider->GetContacts())
        {
            sawMeshContact = true;
            if (contact.otherBody == groundBody &&
                contact.normalWorld.y > 0.5f &&
                contact.normalImpulse >= 0.f)
                sawGroundContact = true;
            sawMappedContact = sawMappedContact || contact.surfaceMapped;
            if (contact.surfaceMapped && contact.boneWeightCount > 0)
            {
                if (frame > 225 && contact.normalImpulse > 0.f)
                    sawPostHandoffWeightedContact = true;
                float weightSum = 0.f;
                for (uint8_t index = 0; index < contact.boneWeightCount; ++index)
                {
                    weightSum += contact.boneWeights[index].weight;
                    sawResolvedBone = sawResolvedBone ||
                        contact.boneWeights[index].bone != nullptr;
                }
                sawWeightedContact = sawWeightedContact ||
                    std::abs(weightSum - 1.f) < 0.001f;
            }
        }
        if (frame == 225)
            for (AnimationBone* bone : skeleton->ResolveBones())
                handoffBoneRotations.push_back(bone && bone->Owner
                    ? bone->Owner->transform.rotation : glm::vec3(0.f));
        if (frame > 225 && !handoffBoneRotations.empty())
        {
            const auto& bones = skeleton->ResolveBones();
            for (size_t index = 0;
                index < bones.size() && index < handoffBoneRotations.size();
                ++index)
                if (bones[index] && bones[index]->Owner &&
                    glm::length(bones[index]->Owner->transform.rotation -
                        handoffBoneRotations[index]) > 0.005f)
                    sawPostHandoffBoneMotion = true;
        }
        meshCollisionActiveEveryFrame = meshCollisionActiveEveryFrame &&
            skeleton->UsesMeshCollider() &&
            body->GetWorldCollisionBounds(minimum, maximum);
        if (!meshCollisionActiveEveryFrame) return 3;
        if (frame == 0)
        {
            initialMinimum = minimum;
            initialMaximum = maximum;
        }
        else if (frame < 60 &&
            (glm::length(minimum - initialMinimum) > 0.01f ||
                glm::length(maximum - initialMaximum) > 0.01f))
            poseChanged = true;
        if (frame == 59)
            initialMeshCollider = skeleton->UsesMeshCollider() &&
                animation->playing && body->bodyType == "Kinematic" &&
                controller->GetInfluence() == 0.f;
        if (frame == 149)
        {
            blendedMeshCollider = skeleton->UsesMeshCollider() &&
                animation->playing && body->bodyType == "Kinematic" &&
                controller->GetInfluence() > 0.1f &&
                controller->GetInfluence() < 0.9f;
            unsigned poseBodies = 0;
            for (auto* bone : fox->GetComponentsInChildren<IKBone>())
                poseBodies += bone->poseOnlyWithMeshCollider &&
                    bone->IsSimulating() ? 1u : 0u;
            poseBonesActiveDuringBlend = poseBodies == 23;
        }
        if (frame == 223 || frame == 225)
        {
            const auto vertices = skinnedMesh->BuildPortalCutTriangleStream(
                &skin->BuildPalette());
            if (vertices.empty()) return 4;
            const auto& vertex = vertices.front();
            const glm::vec3 position = glm::vec3(
                skinnedMesh->Owner->transform.GetWorldMatrix() *
                glm::vec4(vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
            if (frame == 223)
            {
                poseBeforeHandoff = position;
                sampledBeforeHandoff = true;
            }
            else
            {
                poseAfterHandoff = position;
                sampledAfterHandoff = true;
            }
        }
    }
    for (const auto& object : scene.GetObjects())
        if (auto* bone = object->GetComponent<IKBone>();
            bone && bone->IsSimulating())
            ++activeBoneBodies;
    const bool hasBounds = body->GetWorldCollisionBounds(minimum, maximum);
    float lateBoneDrift = 0.f;
    for (size_t index = 0; index < lateBoneRotations.size() &&
        index < finalBoneRotations.size(); ++index)
        lateBoneDrift = std::max(lateBoneDrift,
            glm::length(finalBoneRotations[index] - lateBoneRotations[index]));
    float finalBoneBend = 0.f;
    const auto& finalBones = skeleton->ResolveBones();
    for (size_t index = 0;
        index < finalBones.size() && index < handoffBoneRotations.size();
        ++index)
        if (finalBones[index] && finalBones[index]->Owner)
            finalBoneBend = std::max(finalBoneBend,
                glm::length(finalBones[index]->Owner->transform.rotation -
                    handoffBoneRotations[index]));
    const auto skinnedVertices = skinnedMesh->BuildPortalCutTriangleStream(
        &skin->BuildPalette());
    float meshMinimumY = std::numeric_limits<float>::infinity();
    const glm::mat4 meshWorld = skinnedMesh->Owner->transform.GetWorldMatrix();
    for (const auto& vertex : skinnedVertices)
        meshMinimumY = std::min(meshMinimumY,
            (meshWorld * glm::vec4(vertex.pos[0], vertex.pos[1],
                vertex.pos[2], 1.f)).y);
    glm::vec3 groundMinimum(0.f), groundMaximum(0.f);
    if (!groundBody || !groundBody->GetWorldCollisionBounds(
        groundMinimum, groundMaximum)) return 4;
    auto* groundCollider = ground->GetComponent<PrimitiveObjectCollider>();
    if (!groundCollider) return 4;
    groundCollider->collisionEnabled = false;
    groundCollider->MarkConfigurationDirty();
    probeCollider->collisionEnabled = false;
    probeCollider->MarkConfigurationDirty();
    for (int frame = 0; frame < 120; ++frame)
        scene.Update(1.f / 60.f);
    float settledBoneBend = 0.f;
    AnimationBone* springCandidate = nullptr;
    float springCandidateBend = 0.f;
    for (size_t index = 0;
        index < finalBones.size() && index < handoffBoneRotations.size();
        ++index)
        if (finalBones[index] && finalBones[index]->Owner)
        {
            const float bend = glm::length(
                finalBones[index]->Owner->transform.rotation -
                handoffBoneRotations[index]);
            settledBoneBend = std::max(settledBoneBend,
                bend);
            if (auto* joint = finalBones[index]->Owner->GetComponent<IKBone>();
                joint && joint->connectToParent &&
                finalBones[index]->GetParentBone() &&
                bend > springCandidateBend)
            {
                springCandidateBend = bend;
                springCandidate = finalBones[index];
            }
        }
    if (!springCandidate || springCandidateBend < 0.01f ||
        !collider->GetContacts().empty()) return 4;
    const glm::vec3 beforeSpring = springCandidate->Owner->transform.rotation;
    auto* springJoint = springCandidate->Owner->GetComponent<IKBone>();
    springJoint->jointType = "Spring";
    springJoint->springStiffness = 30.f;
    springJoint->springDamping = 1.f;
    for (int frame = 0; frame < 120; ++frame)
        scene.Update(1.f / 60.f);
    const float springRecovery = glm::length(
        springCandidate->Owner->transform.rotation - beforeSpring);
    std::fprintf(stderr,
        "Fox mesh collider: bounds=%d min=(%.2f, %.2f, %.2f) max=(%.2f, %.2f, %.2f) active bones=%u contact=%d mapped=%d weighted=%d poseChanged=%d finalBend=%.4f settledBend=%.4f springRecovery=%.4f meshY=%.2f floorY=%.2f rootDrift=%.4f boneDrift=%.4f\n",
        hasBounds, minimum.x, minimum.y, minimum.z,
        maximum.x, maximum.y, maximum.z, activeBoneBodies,
        sawMeshContact, sawMappedContact, sawWeightedContact,
        poseChanged, finalBoneBend, settledBoneBend, springRecovery,
        meshMinimumY, groundMaximum.y,
        glm::length(finalRoot - lateRoot), lateBoneDrift);
    return hasBounds && activeBoneBodies == 0 &&
        initialMeshCollider && blendedMeshCollider && meshCollisionActiveEveryFrame &&
        poseBonesActiveDuringBlend &&
        sampledBeforeHandoff && sampledAfterHandoff &&
        glm::length(poseAfterHandoff - poseBeforeHandoff) < 0.5f &&
        touchedFox && sawMeshContact && sawGroundContact && sawMappedContact &&
        sawWeightedContact && sawResolvedBone &&
        sawPostHandoffWeightedContact && sawPostHandoffBoneMotion &&
        poseChanged && finalBoneBend > 0.005f &&
        settledBoneBend > 0.005f && springRecovery > 0.01f &&
        skeleton->UsesMeshCollider() && !animation->playing &&
        body->bodyType == "Dynamic" && body->useGravity &&
        fox->transform.GetWorldPosition().y < startHeight - 0.25f &&
        meshMinimumY >= groundMaximum.y - 0.03f &&
        std::isfinite(initialMinimum.x) &&
        std::isfinite(initialMaximum.x) &&
        initialMaximum.y > initialMinimum.y ? 0 : 3;
}
