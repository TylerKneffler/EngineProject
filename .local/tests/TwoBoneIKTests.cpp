#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Constraints/Subconstraints/AimConstraint.h"
#include "Core/Compoonents/Constraints/Subconstraints/ParentConstraint.h"
#include "Core/Compoonents/Constraints/Subconstraints/PositionConstraint.h"
#include "Core/Compoonents/Constraints/Subconstraints/RotationConstraint.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/ComponentReference.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Model/AnimationControlSpace.h"
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (false)

int main()
{
    using Engine::Components::TwoBoneIK;
    Engine::Core::Object upper, lower, tip;
    lower.Parent = &upper;
    tip.Parent = &lower;
    upper.Children.push_back(&lower);
    lower.Children.push_back(&tip);
    lower.transform.position = { 1.f, 0.f, 0.f };
    tip.transform.position = { 1.f, 0.f, 0.f };

    const glm::vec3 start = tip.transform.GetWorldPosition();
    CHECK(TwoBoneIK::Solve(upper, lower, tip, { 1.f, 1.f, 0.f },
        { 0.f, 1.f, 0.f }, 0.f));
    CHECK(glm::distance(start, tip.transform.GetWorldPosition()) < 1e-4f);

    CHECK(TwoBoneIK::Solve(upper, lower, tip, { 1.f, 1.f, 0.f },
        { 0.f, 1.f, 0.f }));
    CHECK(glm::distance(tip.transform.GetWorldPosition(),
        glm::vec3(1.f, 1.f, 0.f)) < 2e-3f);
    CHECK(glm::distance(lower.transform.GetWorldPosition(),
        glm::vec3(0.f, 1.f, 0.f)) < 2e-3f);

    // An unreachable target clamps to the chain length without introducing
    // NaNs, and a pole on the root-target axis has a deterministic fallback.
    CHECK(TwoBoneIK::Solve(upper, lower, tip, { 5.f, 0.f, 0.f },
        { 6.f, 0.f, 0.f }));
    const glm::vec3 reached = tip.transform.GetWorldPosition();
    CHECK(std::isfinite(reached.x) && std::isfinite(reached.y) &&
        std::isfinite(reached.z));
    CHECK(std::abs(glm::length(reached) - 2.f) < 2e-3f);

    // Crossing full extension retains a finite bend on the pole side.
    CHECK(TwoBoneIK::Solve(upper, lower, tip, { 1.999f, .01f, 0.f },
        { 0.f, 1.f, 0.f }));
    CHECK(std::isfinite(lower.transform.GetWorldPosition().y));
    CHECK(lower.transform.GetWorldPosition().y > 0.f);

    const glm::mat4 target = glm::rotate(glm::translate(glm::mat4(1.f),
        glm::vec3(1.f, 1.f, 0.f)), .5f, glm::vec3(0.f, 0.f, 1.f));
    CHECK(TwoBoneIK::Solve(upper, lower, tip, { 1.f, 1.f, 0.f },
        { 0.f, 1.f, 0.f }, 1.f, 1.f, &target));
    const glm::vec3 desiredX = glm::normalize(glm::vec3(target[0]));
    const glm::vec3 actualX = glm::normalize(glm::vec3(
        tip.transform.GetWorldMatrix()[0]));
    CHECK(glm::dot(desiredX, actualX) > .999f);

    // Runtime component resolves exact scene objects, including targets that
    // live outside the bone hierarchy.
    Engine::Scene::Scene scene;
    auto* runtimeUpper = scene.AddObject("Upper");
    auto* runtimeLower = scene.AddObject("Lower");
    auto* runtimeTip = scene.AddObject("Tip");
    auto* runtimeTarget = scene.AddObject("Target");
    auto* runtimePole = scene.AddObject("Pole");
    CHECK(scene.MoveObject(runtimeLower, runtimeUpper,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    CHECK(scene.MoveObject(runtimeTip, runtimeLower,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    runtimeLower->transform.position = { 1.f, 0.f, 0.f };
    runtimeTip->transform.position = { 1.f, 0.f, 0.f };
    runtimeTarget->transform.position = { 1.f, 1.f, 0.f };
    runtimePole->transform.position = { 0.f, 1.f, 0.f };
    auto* component = runtimeUpper->AddComponent<TwoBoneIK>();
    component->lowerReference = Engine::Core::CaptureComponentReference(
        &runtimeLower->transform, "Transform");
    component->tipReference = Engine::Core::CaptureComponentReference(
        &runtimeTip->transform, "Transform");
    component->targetReference = Engine::Core::CaptureComponentReference(
        &runtimeTarget->transform, "Transform");
    component->poleReference = Engine::Core::CaptureComponentReference(
        &runtimePole->transform, "Transform");
    component->Apply();
    CHECK(glm::distance(runtimeTip->transform.GetWorldPosition(),
        runtimeTarget->transform.GetWorldPosition()) < 2e-3f);
    runtimeUpper->transform.rotation = { 0.f, 0.f, 0.f };
    runtimeLower->transform.rotation = { 0.f, 0.f, 0.f };
    component->positionWeight = .5f;
    component->Start();
    component->Apply();
    const glm::vec3 partial = runtimeTip->transform.GetWorldPosition();
    component->Apply();
    CHECK(glm::distance(partial,
        runtimeTip->transform.GetWorldPosition()) < 1e-4f);

    // A zero FK/IK blend leaves the keyed pose intact; a full blend reaches
    // the IK target. The clip's authoring keys survive serialization.
    upper.transform.rotation = lower.transform.rotation = tip.transform.rotation = {};
    CHECK(TwoBoneIK::Blend(upper, lower, tip, {1.f, 1.f, 0.f},
        {0.f, 1.f, 0.f}, glm::quat(1.f, 0.f, 0.f, 0.f), 0.f));
    CHECK(glm::distance(tip.transform.GetWorldPosition(), start) < 1e-4f);
    const glm::vec3 matchedPole = lower.transform.GetWorldPosition() * 2.f -
        (upper.transform.GetWorldPosition() + start) * .5f;
    CHECK(TwoBoneIK::Blend(upper, lower, tip, start, matchedPole,
        glm::quat(1.f, 0.f, 0.f, 0.f), 1.f));
    CHECK(glm::distance(tip.transform.GetWorldPosition(), start) < 2e-3f);
    upper.transform.rotation = lower.transform.rotation = tip.transform.rotation = {};
    CHECK(TwoBoneIK::Blend(upper, lower, tip, {1.f, 1.f, 0.f},
        {0.f, 1.f, 0.f}, glm::quat(1.f, 0.f, 0.f, 0.f), 1.f));
    CHECK(glm::distance(tip.transform.GetWorldPosition(),
        glm::vec3(1.f, 1.f, 0.f)) < 2e-3f);
    Engine::Model::AnimationClip clip;
    Engine::Model::AnimationClip::LimbTrack limb;
    limb.upper = 1; limb.lower = 2; limb.tip = 3;
    auto first = Engine::Model::AnimationClip::LimbKey{};
    first.time = 0.f; first.blend = 0.f;
    auto second = first;
    second.time = 1.f; second.target = {1.f, 1.f, 0.f};
    second.blend = 1.f;
    limb.keys = {first, second};
    clip.limbs.push_back(limb);
    Engine::Model::AnimationClip restored;
    restored.Deserialize(clip.Serialize());
    CHECK(restored.limbs.size() == 1 && restored.limbs[0].tip == 3);
    const auto halfway = Engine::Model::AnimationClip::SampleLimb(
        restored.limbs[0], .5f);
    CHECK(std::abs(halfway.blend - .5f) < 1e-5f);
    CHECK(std::abs(halfway.target.x - .5f) < 1e-5f);
    // Space changes are keyed events. A reference-frame switch retains the
    // outgoing control up to the switch frame and round-trips its prop index.
    auto switched = second;
    switched.time = 2.f;
    switched.space = Engine::Model::AnimationClip::LimbKey::Space::Prop;
    switched.spaceNode = 7;
    limb.keys.push_back(switched);
    clip.limbs[0] = limb;
    restored.Deserialize(clip.Serialize());
    CHECK(restored.limbs[0].keys.back().space ==
        Engine::Model::AnimationClip::LimbKey::Space::Prop);
    CHECK(restored.limbs[0].keys.back().spaceNode == 7);
    CHECK(Engine::Model::AnimationClip::SampleLimb(
        restored.limbs[0], 1.5f).space ==
        Engine::Model::AnimationClip::LimbKey::Space::Body);
    CHECK(Engine::Model::AnimationClip::SampleLimb(
        restored.limbs[0], 2.f).space ==
        Engine::Model::AnimationClip::LimbKey::Space::Prop);
    Engine::Scene::Scene spaces;
    auto* bodySpace = spaces.AddObject("Body space");
    auto* upperSpace = spaces.AddObject("Upper space");
    auto* propSpace = spaces.AddObject("Prop space");
    bodySpace->transform.position = {2.f, 0.f, 0.f};
    upperSpace->transform.position = {0.f, 3.f, 0.f};
    propSpace->transform.position = {0.f, 0.f, 4.f};
    const glm::vec3 worldControl(5.f, 6.f, 7.f);
    const std::vector<Engine::Core::Object*> spaceNodes{upperSpace};
    auto spaceKey = Engine::Model::AnimationClip::LimbKey{};
    for (auto space : {Engine::Model::AnimationClip::LimbKey::Space::Body,
            Engine::Model::AnimationClip::LimbKey::Space::Local,
            Engine::Model::AnimationClip::LimbKey::Space::World,
            Engine::Model::AnimationClip::LimbKey::Space::Prop})
    {
        spaceKey.space = space;
        if (space == Engine::Model::AnimationClip::LimbKey::Space::Prop)
            spaceKey.propReference = Engine::Core::CaptureComponentReference(
                &propSpace->transform, "Transform");
        const glm::mat4 matrix = Engine::Model::LimbSpaceMatrix(spaceKey,
            bodySpace->transform.GetWorldMatrix(), *upperSpace, spaceNodes);
        const glm::vec3 local = glm::vec3(glm::inverse(matrix) *
            glm::vec4(worldControl, 1.f));
        CHECK(glm::distance(Engine::Model::LimbWorldPoint(matrix, local),
            worldControl) < 1e-5f);
    }
    spaceKey.time = 2.f;
    clip.limbs[0].keys.back() = spaceKey;
    restored.Deserialize(clip.Serialize());
    CHECK(restored.limbs[0].keys.back().propReference.objectName == "Prop space");

    // Scene constraints resolve targets before dependents and never evaluate
    // a cycle. Repeated evaluation must not accumulate partial weights.
    Engine::Scene::Scene constrained;
    auto* head = constrained.AddObject("Head");
    auto* look = constrained.AddObject("Look target");
    look->transform.position = { 5.f, 0.f, 5.f };
    auto* aim = head->AddComponent<Engine::Components::AimConstraint>();
    aim->targetReference = Engine::Core::CaptureComponentReference(
        &look->transform, "Transform");
    constrained.ApplyPoseConstraints();
    const glm::vec3 forward = glm::normalize(glm::vec3(
        head->transform.GetWorldMatrix()[2]));
    CHECK(glm::dot(forward, glm::normalize(look->transform.position)) > .999f);
    auto* follower = constrained.AddObject("Follower");
    auto* position = follower->AddComponent<Engine::Components::PositionConstraint>();
    position->positionWeight = .5f;
    position->targetReference = Engine::Core::CaptureComponentReference(
        &look->transform, "Transform");
    Engine::Components::PositionConstraint restoredPosition;
    restoredPosition.Deserialize(position->Serialize());
    CHECK(restoredPosition.GetTypeName() == "PositionConstraint");
    CHECK(std::abs(restoredPosition.positionWeight - .5f) < 1e-5f);
    CHECK(restoredPosition.targetReference.IsAssigned());
    constrained.ApplyPoseConstraints();
    CHECK(std::abs(follower->transform.position.x - 2.5f) < 1e-4f);
    auto* parented = constrained.AddObject("Parented prop");
    auto* parentConstraint = parented->AddComponent<
        Engine::Components::ParentConstraint>();
    parentConstraint->targetReference = Engine::Core::CaptureComponentReference(
        &look->transform, "Transform");
    look->transform.rotation.z = .5f;
    constrained.ApplyPoseConstraints();
    CHECK(glm::distance(parented->transform.GetWorldPosition(),
        look->transform.GetWorldPosition()) < 1e-4f);
    CHECK(std::abs(parented->transform.rotation.z - .5f) < 1e-4f);
    auto* rotated = constrained.AddObject("Rotated prop");
    rotated->transform.position.x = -3.f;
    auto* rotationConstraint = rotated->AddComponent<
        Engine::Components::RotationConstraint>();
    rotationConstraint->targetReference = Engine::Core::CaptureComponentReference(
        &look->transform, "Transform");
    constrained.ApplyPoseConstraints();
    CHECK(std::abs(rotated->transform.rotation.z - .5f) < 1e-4f);
    CHECK(std::abs(rotated->transform.position.x + 3.f) < 1e-4f);
    constrained.ApplyPoseConstraints();
    CHECK(std::abs(follower->transform.position.x - 2.5f) < 1e-4f);
    auto* cycleA = constrained.AddObject("Cycle A");
    auto* cycleB = constrained.AddObject("Cycle B");
    cycleA->transform.position.x = 1.f;
    cycleB->transform.position.x = 2.f;
    auto* a = cycleA->AddComponent<Engine::Components::PositionConstraint>();
    auto* b = cycleB->AddComponent<Engine::Components::PositionConstraint>();
    a->targetReference = Engine::Core::CaptureComponentReference(
        &cycleB->transform, "Transform");
    b->targetReference = Engine::Core::CaptureComponentReference(
        &cycleA->transform, "Transform");
    constrained.ApplyPoseConstraints();
    CHECK(std::abs(cycleA->transform.position.x - 1.f) < 1e-4f);
    CHECK(std::abs(cycleB->transform.position.x - 2.f) < 1e-4f);
    return 0;
}
