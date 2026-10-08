#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Scripts/Physics/FoxRagdollBlend.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <glm/gtc/quaternion.hpp>

namespace
{
bool CheckTransformedPerBoneFox()
{
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_ragdoll.scene", nullptr))
        return false;
    auto* fox = scene.FindObjectByName("Fox");
    auto* ground = scene.FindObjectByName("Ragdoll Ground");
    auto* meshCollider = fox ? fox->GetComponent<
        Engine::Components::MeshObjectCollider>() : nullptr;
    auto* mesh = meshCollider
        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(
            fox, meshCollider->meshReference) : nullptr;
    auto* skin = mesh && mesh->Owner
        ? mesh->Owner->GetComponent<Engine::Components::SkinnedMesh>() : nullptr;
    auto* groundBody = ground
        ? ground->GetComponent<Engine::Components::RigidBody>() : nullptr;
    if (!fox || !mesh || !skin || !groundBody) return false;
    fox->transform.rotation.y += 0.35f;
    fox->transform.scale *= 1.15f;
    fox->transform.MarkDirty();
    scene.Start();
    float worstClearance = std::numeric_limits<float>::infinity();
    for (int frame = 0; frame < 900; ++frame)
    {
        scene.Update(1.f / 60.f);
        if (frame < 300 || frame % 30 != 0) continue;
        glm::vec3 groundMinimum(0.f), groundMaximum(0.f);
        if (!groundBody->GetWorldCollisionBounds(groundMinimum,
                groundMaximum)) return false;
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
    std::fprintf(stderr, "Transformed per-bone fox: clearance=%.3f\n",
        worstClearance);
    return std::isfinite(worstClearance) && worstClearance >= -0.05f;
}
}

int main()
{
    Engine::Serialization::RegisterComponentType<FoxRagdollBlend>(
        "FoxRagdollBlend");
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_ragdoll.scene", nullptr))
        return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* controllerObject = scene.FindObjectByName(
        "Fox Ragdoll Blend Controller");
    auto* controller = controllerObject
        ? controllerObject->GetComponent<FoxRagdollBlend>() : nullptr;
    auto* skeleton = fox
        ? fox->GetComponent<Engine::Components::Skeleton>() : nullptr;
    auto* animation = fox
        ? fox->GetComponent<Engine::Components::AnimationManager>() : nullptr;
    auto* body = fox
        ? fox->GetComponent<Engine::Components::RigidBody>() : nullptr;
    auto bones = fox
        ? fox->GetComponentsInChildren<Engine::Components::IKBone>()
        : std::vector<Engine::Components::IKBone*>{};
    auto* meshCollider = fox ? fox->GetComponent<
        Engine::Components::MeshObjectCollider>() : nullptr;
    auto* mesh = meshCollider
        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(
            fox, meshCollider->meshReference) : nullptr;
    auto* skin = mesh && mesh->Owner
        ? mesh->Owner->GetComponent<Engine::Components::SkinnedMesh>() : nullptr;
    auto* ground = scene.FindObjectByName("Ragdoll Ground");
    auto* groundBody = ground
        ? ground->GetComponent<Engine::Components::RigidBody>() : nullptr;
    if (!controller || !skeleton || !animation || !body || bones.size() != 23 ||
        !mesh || !skin || !groundBody ||
        animation->clips.size() != 3 ||
        !animation->FindClip("Survey") ||
        !animation->FindClip("Walk") ||
        !animation->FindClip("Run"))
        return 2;

    scene.Start();
    glm::vec3 groundMinimum(0.f), groundMaximum(0.f);
    if (skeleton->UsesMeshCollider() || !animation->playing ||
        body->bodyType != "Kinematic" || body->useGravity)
        return 3;
    const float startHeight = fox->transform.GetWorldPosition().y;
    float initial = -1.f, mixed = -1.f, complete = -1.f;
    float initialBone = -1.f, mixedBone = -1.f, completeBone = -1.f;
    unsigned initialBodies = 0, mixedBodies = 0, completeBodies = 0;
    bool animationPlayingDuringBlend = false;
    std::vector<glm::vec3> positionsStart, positionsPrevious, positionsEnd;
    std::vector<glm::quat> rotationsStart, rotationsPrevious, rotationsEnd;
    std::vector<glm::quat> handoffRotations;
    glm::vec3 rootStart(0.f), rootEnd(0.f);
    float worstVisibleClearance = std::numeric_limits<float>::infinity();
    for (int frame = 1; frame <= 1800; ++frame)
    {
        scene.Update(1.f / 60.f);
        if (frame == 225)
            for (auto* bone : bones)
                handoffRotations.emplace_back(bone->Owner->transform.rotation);
        if (frame >= 300 && frame % 30 == 0)
        {
            if (!groundBody->GetWorldCollisionBounds(
                    groundMinimum, groundMaximum)) return 3;
            const auto vertices = mesh->BuildPortalCutTriangleStream(
                &skin->BuildPalette());
            const glm::mat4 meshWorld =
                mesh->Owner->transform.GetWorldMatrix();
            for (const auto& vertex : vertices)
            {
                const glm::vec3 point(meshWorld * glm::vec4(vertex.pos[0],
                    vertex.pos[1], vertex.pos[2], 1.f));
                if (point.x >= groundMinimum.x &&
                    point.x <= groundMaximum.x &&
                    point.z >= groundMinimum.z &&
                    point.z <= groundMaximum.z)
                    worstVisibleClearance = std::min(worstVisibleClearance,
                        point.y - groundMaximum.y);
            }
        }
        if (frame == 1500 || frame == 1799 || frame == 1800)
        {
            auto& positions = frame == 1500 ? positionsStart
                : frame == 1799 ? positionsPrevious : positionsEnd;
            auto& rotations = frame == 1500 ? rotationsStart
                : frame == 1799 ? rotationsPrevious : rotationsEnd;
            for (auto* bone : bones)
            {
                positions.push_back(bone->Owner->transform.GetWorldPosition());
                rotations.emplace_back(bone->Owner->transform.rotation);
            }
            if (frame == 1500) rootStart = fox->transform.GetWorldPosition();
            if (frame == 1800) rootEnd = fox->transform.GetWorldPosition();
        }
        if (frame != 60 && frame != 150 && frame != 300)
            continue;
        unsigned bodies = 0;
        for (auto* bone : bones)
            bodies += bone->IsSimulating() ? 1u : 0u;
        if (frame == 60)
        {
            initial = controller->GetInfluence();
            initialBone = bones.front()->GetInfluence();
            initialBodies = bodies;
        }
        else if (frame == 150)
        {
            mixed = controller->GetInfluence();
            mixedBone = bones.front()->GetInfluence();
            mixedBodies = bodies;
            animationPlayingDuringBlend = animation->playing;
        }
        else
        {
            complete = controller->GetInfluence();
            completeBone = bones.front()->GetInfluence();
            completeBodies = bodies;
        }
    }
    const float endHeight = fox->transform.GetWorldPosition().y;
    float maxBoneDrift = 0.f, maxFinalFrameMove = 0.f;
    float maxRotationDrift = 0.f, maxFinalFrameRotation = 0.f;
    unsigned bentBones = 0;
    float maximumBend = 0.f;
    for (size_t index = 0; index < bones.size(); ++index)
    {
        maxBoneDrift = std::max(maxBoneDrift,
            glm::length(positionsEnd[index] - positionsStart[index]));
        maxFinalFrameMove = std::max(maxFinalFrameMove,
            glm::length(positionsEnd[index] - positionsPrevious[index]));
        const auto angleBetween = [](const glm::quat& a, const glm::quat& b)
        {
            return 2.f * std::acos(std::clamp(
                std::abs(glm::dot(a, b)), 0.f, 1.f));
        };
        maxRotationDrift = std::max(maxRotationDrift,
            angleBetween(rotationsEnd[index], rotationsStart[index]));
        maxFinalFrameRotation = std::max(maxFinalFrameRotation,
            angleBetween(rotationsEnd[index], rotationsPrevious[index]));
        if (index < handoffRotations.size())
        {
            const float bend = angleBetween(rotationsEnd[index],
                handoffRotations[index]);
            bentBones += bend > 0.1f ? 1u : 0u;
            maximumBend = std::max(maximumBend, bend);
        }
    }
    if (!groundBody->GetWorldCollisionBounds(
        groundMinimum, groundMaximum)) return 5;
    float deepestFloorContact = 0.f;
    unsigned floorContactBones = 0;
    unsigned activeBodies = 0;
    unsigned fittedBodies = 0;
    bool rootPrimitiveFallback = false;
    for (auto* bone : bones)
    {
        if (!bone->IsSimulating()) continue;
        ++activeBodies;
        fittedBodies += bone->IsUsingSkinnedCollider() ? 1u : 0u;
        if (!bone->IsUsingSkinnedCollider())
        {
            rootPrimitiveFallback = rootPrimitiveFallback ||
                bone->Owner->name == "b_Root_00";
            std::fprintf(stderr, "Primitive fallback: %s\n",
                bone->Owner->name.c_str());
        }
        float contactSeparation = 0.f;
        if (bone->GetContactSeparation(groundBody, contactSeparation))
        {
            ++floorContactBones;
            deepestFloorContact = std::min(deepestFloorContact,
                contactSeparation);
        }
    }
    const auto vertices = mesh->BuildPortalCutTriangleStream(
        &skin->BuildPalette());
    float lowestMeshOverGround = std::numeric_limits<float>::infinity();
    Engine::Components::Mesh::Vertex lowestOverGroundVertex {};
    const glm::mat4 meshWorld = mesh->Owner->transform.GetWorldMatrix();
    for (const auto& vertex : vertices)
    {
        const glm::vec3 point(meshWorld * glm::vec4(vertex.pos[0],
            vertex.pos[1], vertex.pos[2], 1.f));
        if (point.x >= groundMinimum.x && point.x <= groundMaximum.x &&
            point.z >= groundMinimum.z && point.z <= groundMaximum.z &&
            point.y < lowestMeshOverGround)
        {
            lowestMeshOverGround = point.y;
            lowestOverGroundVertex = vertex;
        }
    }
    const auto& paletteBones = skeleton->ResolveBones();
    const char* lowestMeshBone = "missing";
    float strongestWeight = 0.f;
    Engine::Components::AnimationBone* lowestWeightBone = nullptr;
    for (int i = 0; i < 4; ++i)
    {
        const int palette = static_cast<int>(lowestOverGroundVertex.joints0[i]);
        if (palette >= 0 && static_cast<size_t>(palette) < paletteBones.size() &&
            lowestOverGroundVertex.weights0[i] > strongestWeight)
        {
            strongestWeight = lowestOverGroundVertex.weights0[i];
            if (paletteBones[palette] && paletteBones[palette]->Owner)
            {
                lowestMeshBone = paletteBones[palette]->Owner->name.c_str();
                lowestWeightBone = paletteBones[palette];
            }
        }
    }
    if (lowestWeightBone && lowestWeightBone->Owner)
    {
        const glm::vec3 point(meshWorld * glm::vec4(
            lowestOverGroundVertex.pos[0],
            lowestOverGroundVertex.pos[1],
            lowestOverGroundVertex.pos[2], 1.f));
        const glm::vec3 bonePoint =
            lowestWeightBone->Owner->transform.GetWorldPosition();
        std::fprintf(stderr,
            "Lowest vertex offset from %s: (%.3f, %.3f, %.3f)\n",
            lowestMeshBone, point.x - bonePoint.x,
            point.y - bonePoint.y, point.z - bonePoint.z);
    }
    std::fprintf(stderr,
        "Fox blend: animation=%.2f/%u, mixed=%.2f/%u, ragdoll=%.2f/%u, rootY=%.2f -> %.2f, floorY=%.3f activeIK=%u fitted=%u floorContacts=%u deepest=%.3f visibleMinY=%.3f (%s), worstClearance=%.3f rootDrift=%.3f boneDrift=%.3f boneAngle=%.3f bentBones=%u maxBend=%.3f finalMove=%.5f finalAngle=%.5f\n",
        initial, initialBodies, mixed, mixedBodies, complete,
        completeBodies, startHeight, endHeight, groundMaximum.y,
        activeBodies, fittedBodies, floorContactBones, deepestFloorContact,
        lowestMeshOverGround, lowestMeshBone, worstVisibleClearance,
        glm::length(rootEnd - rootStart), maxBoneDrift,
        maxRotationDrift, bentBones, maximumBend,
        maxFinalFrameMove, maxFinalFrameRotation);
    return initial == 0.f && initialBone == 0.f && initialBodies == 0 &&
        mixed > 0.1f && mixed < 0.9f &&
        std::abs(mixedBone - mixed) < 0.001f &&
        mixedBodies == 23 && animationPlayingDuringBlend &&
        complete == 1.f && completeBone == 1.f && completeBodies == 23 &&
        !animation->playing && !skeleton->UsesMeshCollider() &&
        body->bodyType == "Kinematic" && !body->useGravity &&
        activeBodies == 23 && fittedBodies == 22 &&
        rootPrimitiveFallback &&
        floorContactBones > 0 &&
        deepestFloorContact >= -0.03f &&
        worstVisibleClearance >= -0.05f &&
        std::isfinite(endHeight) &&
        endHeight < startHeight - 0.25f &&
        CheckTransformedPerBoneFox() ? 0 : 4;
}
