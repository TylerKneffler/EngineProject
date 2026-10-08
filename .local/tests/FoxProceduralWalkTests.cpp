#include "Scripts/Physics/FoxProceduralWalk.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/GroundedFootIK.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

int main()
{
    using namespace Engine::Components;
    Engine::Serialization::RegisterComponentType<FoxProceduralWalk>(
        "FoxProceduralWalk");
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/fox_procedural_walk.scene",
            nullptr)) return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* controller = scene.FindObjectByName("Fox Procedural Walk Controller");
    auto* camera = scene.FindObjectByName("Walk Camera");
    auto* floorA = scene.FindObjectByName("Walk Floor A");
    auto* floorB = scene.FindObjectByName("Walk Floor B");
    auto* lowRise = scene.FindObjectByName("Low Walk Rise");
    auto* highRise = scene.FindObjectByName("High Walk Rise");
    auto* unevenRise = scene.FindObjectByName("Uneven Walk Rise");
    auto* walker = controller
        ? controller->GetComponent<FoxProceduralWalk>() : nullptr;
    auto* footIK = controller
        ? controller->GetComponent<GroundedFootIK>() : nullptr;
    auto* animation = fox ? fox->GetComponent<AnimationManager>() : nullptr;
    auto* body = fox ? fox->GetComponent<RigidBody>() : nullptr;
    auto* foot = fox ? fox->FindObjectInChildrenByName(
        "b_RightHand_08") : nullptr;
    if (!fox || !controller || !camera || !floorA || !floorB ||
        !lowRise || !highRise || !unevenRise || !walker || !footIK ||
        !animation || !body || !foot)
        return 2;
    const auto* walkClip = animation->FindClip("Walk");
    if (animation->clips.size() != 3 ||
        animation->GetAvailableClips().size() != 3 ||
        !animation->FindClip("Survey") || !animation->FindClip("Run") ||
        !walkClip || walkClip->channels.empty() ||
        walkClip != &animation->clips[1]) return 2;
    AnimationManager clipRoundTrip;
    clipRoundTrip.clips.push_back(*walkClip);
    AnimationManager restoredClips;
    restoredClips.Deserialize(clipRoundTrip.Serialize());
    const auto* restoredWalk = restoredClips.FindClip("Walk");
    if (!restoredWalk || restoredWalk->channels.size() !=
        walkClip->channels.size() ||
        restoredWalk->duration != walkClip->duration) return 2;
    Engine::Scene::Scene clipScene;
    auto* clipObject = clipScene.AddObject("Clip Owner");
    clipObject->AddComponent<AnimationManager>()->clips.push_back(
        *restoredWalk);
    Engine::Scene::Scene loadedClipScene;
    const std::string serializedClipScene =
        Engine::Serialization::SceneSerializer::SaveToString(clipScene);
    if (!Engine::Serialization::SceneSerializer::LoadFromString(
            loadedClipScene,
            serializedClipScene,
            nullptr)) return 2;
    auto* loadedClipObject = loadedClipScene.FindObjectByName("Clip Owner");
    auto* loadedManager = loadedClipObject
        ? loadedClipObject->GetComponent<AnimationManager>() : nullptr;
    if (!loadedManager || !loadedManager->FindClip("Walk") ||
        loadedManager->FindClip("Walk")->channels.size() !=
            walkClip->channels.size()) return 2;
    float walkDuration = 0.f;
    if (const auto* walk = animation->FindClip("Walk"))
        walkDuration = walk->duration;
    if (walkDuration <= 0.f) return 2;
    const size_t objectCount = scene.GetObjects().size();
    // Traverse the course quickly while keeping the clip at walking speed.
    walker->forwardSpeed = 2.f;
    walker->strideDistance = walker->forwardSpeed * walkDuration / 0.7f;
    scene.Start();
    for (auto* bone : fox->GetComponentsInChildren<IKBone>())
        if (bone->simulate || bone->GetInfluence() > 0.f)
            return 2;
    const glm::vec3 initial = fox->transform.GetWorldPosition();
    const glm::vec3 cameraInitial = camera->transform.GetWorldPosition();
    glm::vec3 firstRelativeFoot(0.f);
    float maxRelativeFootMotion = 0.f;
    float minRootY = initial.y, maxRootY = initial.y;
    float maxCyclePhaseError = 0.f;
    double updateMilliseconds = 0.0;
    const auto trackPhase = [&]()
    {
        const float distancePhase = std::fmod(
            walker->GetTravel() / walker->strideDistance, 1.f);
        const float clipPhase = animation->time / walkDuration;
        const float difference = std::abs(distancePhase - clipPhase);
        maxCyclePhaseError = std::max(maxCyclePhaseError,
            std::min(difference, 1.f - difference));
    };
    float lowArrivalError = 100.f, highArrivalError = 100.f;
    float unevenArrivalError = 100.f;
    const std::array<Engine::Core::Object*, 3> rises {
        lowRise, highRise, unevenRise
    };
    std::array<float, 3> bestPawHeightError { 100.f, 100.f, 100.f };
    for (int frame = 0; frame < 360; ++frame)
    {
        const auto begin = std::chrono::steady_clock::now();
        scene.Update(1.f / 60.f);
        if (frame >= 60)
            updateMilliseconds += std::chrono::duration<double,
                std::milli>(std::chrono::steady_clock::now() - begin).count();
        trackPhase();
        const glm::vec3 root = fox->transform.GetWorldPosition();
        const glm::vec3 relativeFoot =
            foot->transform.GetWorldPosition() - root;
        if (frame == 0) firstRelativeFoot = relativeFoot;
        maxRelativeFootMotion = std::max(maxRelativeFootMotion,
            glm::distance(relativeFoot, firstRelativeFoot));
        minRootY = std::min(minRootY, root.y);
        maxRootY = std::max(maxRootY, root.y);
        for (unsigned riseIndex = 0; riseIndex < rises.size(); ++riseIndex)
        {
            const auto* rise = rises[riseIndex];
            const glm::vec3 center = rise->transform.GetWorldPosition();
            const glm::vec3 size = rise->transform.scale;
            const float top = center.y + size.y * 0.5f;
            for (unsigned pawIndex = 0; pawIndex < 4; ++pawIndex)
            {
                const auto* paw = footIK->GetLeg(pawIndex).foot;
                const auto& leg = footIK->GetLeg(pawIndex);
                const glm::vec3 position = paw->transform.GetWorldPosition();
                if (leg.planted && leg.supportBody &&
                    leg.supportBody->Owner == rise &&
                    std::abs(position.x - center.x) < size.x * 0.5f &&
                    std::abs(position.z - center.z) < size.z * 0.5f)
                    bestPawHeightError[riseIndex] = std::min(
                        bestPawHeightError[riseIndex],
                        std::abs(position.y - top));
            }
        }
        if (frame == 119)
        {
            lowArrivalError = std::abs(root.z -
                lowRise->transform.GetWorldPosition().z);
        }
        if (frame == 239)
        {
            highArrivalError = std::abs(root.z -
                highRise->transform.GetWorldPosition().z);
        }
        if (frame == 359)
        {
            unevenArrivalError = std::abs(root.z -
                unevenRise->transform.GetWorldPosition().z);
        }
    }
    const glm::vec3 afterCourse = fox->transform.GetWorldPosition();
    const float firstRiseAfterPass =
        lowRise->transform.GetWorldPosition().z;
    walker->forwardSpeed = 8.f;
    walker->strideDistance = walker->forwardSpeed * walkDuration / 0.7f;
    for (int frame = 0; frame < 180; ++frame)
    {
        scene.Update(1.f / 60.f);
        trackPhase();
    }
    glm::vec3 floorMin(0.f), floorMax(0.f);
    auto* floorBBody = floorB->GetComponent<RigidBody>();
    const bool floorHasBounds = floorBBody &&
        floorBBody->GetWorldCollisionBounds(floorMin, floorMax);
    const float repeatedRootZ = fox->transform.GetWorldPosition().z;
    const float repeatedFloorAZ = floorA->transform.GetWorldPosition().z;
    walker->forwardSpeed = 12.f;
    walker->strideDistance = walker->forwardSpeed * walkDuration / 0.7f;
    for (int frame = 0; frame < 240; ++frame)
    {
        scene.Update(1.f / 60.f);
        trackPhase();
    }
    auto* floorABody = floorA->GetComponent<RigidBody>();
    glm::vec3 secondFloorMin(0.f), secondFloorMax(0.f);
    const bool secondFloorHasBounds = floorABody &&
        floorABody->GetWorldCollisionBounds(secondFloorMin,
            secondFloorMax);
    const float secondRootZ = fox->transform.GetWorldPosition().z;
    const float secondFloorBZ = floorB->transform.GetWorldPosition().z;
    std::fprintf(stderr,
        "Fox walk with foot IK: travel=%.2f rootZ=%.2f animatedFootMotion=%.3f rootY=%.3f..%.3f phaseError=%.5f update=%.2fms\n",
        walker->GetTravel(), afterCourse.z, maxRelativeFootMotion,
        minRootY, maxRootY, maxCyclePhaseError,
        updateMilliseconds / 300.0);
    std::fprintf(stderr,
        "Fox paw height errors on platforms: %.3f %.3f %.3f\n",
        bestPawHeightError[0], bestPawHeightError[1],
        bestPawHeightError[2]);
    std::fprintf(stderr, "Fox foot IK: plants=%llu,%llu,%llu,%llu\n",
        static_cast<unsigned long long>(footIK->GetLeg(0).plantSequence),
        static_cast<unsigned long long>(footIK->GetLeg(1).plantSequence),
        static_cast<unsigned long long>(footIK->GetLeg(2).plantSequence),
        static_cast<unsigned long long>(footIK->GetLeg(3).plantSequence));
    const bool coursePassed = animation->playing &&
        animation->clip == "Walk" && body->bodyType == "Kinematic" &&
        std::abs(walker->GetGaitPeriod() -
            walkDuration / animation->speed) < 0.001f &&
        maxCyclePhaseError < 0.01f && maxRelativeFootMotion > 0.05f &&
        maxRootY - minRootY < 0.01f &&
        afterCourse.z < initial.z - 8.f &&
        camera->transform.GetWorldPosition().z < cameraInitial.z - 34.f &&
        lowArrivalError < 0.02f && highArrivalError < 0.02f &&
        unevenArrivalError < 0.02f && firstRiseAfterPass < -18.f &&
        bestPawHeightError[0] < 0.08f &&
        bestPawHeightError[1] < 0.08f &&
        bestPawHeightError[2] < 0.08f &&
        footIK->GetLeg(0).plantSequence > 1 &&
        footIK->GetLeg(1).plantSequence > 1 &&
        footIK->GetLeg(2).plantSequence > 1 &&
        footIK->GetLeg(3).plantSequence > 1 &&
        repeatedRootZ < -34.f && repeatedFloorAZ < -90.f &&
        floorHasBounds && repeatedRootZ > floorMin.z + 2.f &&
        repeatedRootZ < floorMax.z - 2.f &&
        secondRootZ < -82.f && secondFloorBZ < -140.f &&
        secondFloorHasBounds &&
        secondRootZ > secondFloorMin.z + 2.f &&
        secondRootZ < secondFloorMax.z - 2.f &&
        scene.GetObjects().size() == objectCount;
    const float travelBeforePause = walker->GetTravel();
    const float clipTimeBeforePause = animation->time;
    walker->forwardSpeed = 0.f;
    for (int frame = 0; frame < 30; ++frame)
        scene.Update(1.f / 60.f);
    const bool pausePassed =
        std::abs(walker->GetTravel() - travelBeforePause) < 0.0001f &&
        std::abs(animation->time - clipTimeBeforePause) < 0.0001f &&
        animation->speed == 0.f;

    Engine::Scene::Scene normalScene;
    if (!Engine::Serialization::SceneSerializer::Load(normalScene,
            "Engine/Core/Assets/Scenes/Physics/fox_procedural_walk.scene",
            nullptr)) return 4;
    auto* normalFox = normalScene.FindObjectByName("Fox");
    auto* normalController = normalScene.FindObjectByName(
        "Fox Procedural Walk Controller");
    auto* normalRise = normalScene.FindObjectByName("Low Walk Rise");
    auto* normalCamera = normalScene.FindObjectByName("Walk Camera");
    auto* normalFloor = normalScene.FindObjectByName("Walk Floor A");
    auto* normalWalker = normalController
        ? normalController->GetComponent<FoxProceduralWalk>() : nullptr;
    auto* normalFootIK = normalController
        ? normalController->GetComponent<GroundedFootIK>() : nullptr;
    auto* normalAnimation = normalFox
        ? normalFox->GetComponent<AnimationManager>() : nullptr;
    auto* plantedFoot = normalFox
        ? normalFox->FindObjectInChildrenByName("b_RightFoot02_022")
        : nullptr;
    if (!normalFox || !normalRise || !normalCamera || !normalFloor ||
        !normalWalker || !normalFootIK || !normalAnimation || !plantedFoot)
        return 4;
    normalScene.Start();
    const float cameraStartZ = normalCamera->transform.GetWorldPosition().z;
    const float floorStartZ = normalFloor->transform.GetWorldPosition().z;
    glm::vec3 contactBegin(0.f), contactEnd(0.f);
    float normalLowArrivalError = 100.f;
    bool foxMovesBeforeCamera = false;
    for (int frame = 0; frame < 120; ++frame)
    {
        normalScene.Update(1.f / 60.f);
        if (frame == 10)
            contactBegin = plantedFoot->transform.GetWorldPosition();
        if (frame == 25)
            contactEnd = plantedFoot->transform.GetWorldPosition();
        if (frame == 29)
            foxMovesBeforeCamera =
                normalWalker->GetTravel() > 1.3f &&
                std::abs(normalCamera->transform.GetWorldPosition().z -
                    cameraStartZ) < 0.001f &&
                std::abs(normalFloor->transform.GetWorldPosition().z -
                    floorStartZ) < 0.001f;
        if (frame == 89)
            normalLowArrivalError = std::abs(
                normalFox->transform.GetWorldPosition().z -
                normalRise->transform.GetWorldPosition().z);
    }
    const float contactSlip = glm::length(glm::vec2(
        contactEnd.x - contactBegin.x,
        contactEnd.z - contactBegin.z));
    std::fprintf(stderr,
        "Fox gait match: stride=%.2f speed=%.2f contactSlip=%.3f lowArrival=%.3f\n",
        normalWalker->strideDistance, normalWalker->forwardSpeed,
        contactSlip, normalLowArrivalError);
    const bool normalArrival =
        std::abs(normalWalker->GetTravel() - 5.34f) < 0.02f &&
        normalLowArrivalError < 0.05f && contactSlip < 0.2f &&
        foxMovesBeforeCamera &&
        std::abs(normalCamera->transform.GetWorldPosition().z -
            (cameraStartZ - 3.34f)) < 0.02f &&
        std::abs(normalFloor->transform.GetWorldPosition().z -
            floorStartZ) < 0.001f;
    const float beforeSpeedChange = normalWalker->GetTravel();
    normalWalker->forwardSpeed = 1.335f;
    for (int frame = 0; frame < 60; ++frame)
        normalScene.Update(1.f / 60.f);
    const float distancePhase = std::fmod(normalWalker->GetTravel() /
        normalWalker->strideDistance, 1.f);
    const float clipPhase = normalAnimation->time / walkDuration;
    const float phaseDifference = std::abs(distancePhase - clipPhase);
    const bool speedChangePassed =
        std::abs(normalWalker->GetTravel() - beforeSpeedChange - 1.335f)
            < 0.01f &&
        std::min(phaseDifference, 1.f - phaseDifference) < 0.01f &&
        std::abs(normalAnimation->speed * normalWalker->strideDistance /
            walkDuration - 1.335f) < 0.001f;
    return coursePassed && pausePassed && normalArrival &&
        speedChangePassed ? 0 : 3;
}
