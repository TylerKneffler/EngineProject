#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/ComponentReference.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include <cmath>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (false)

int main()
{
    Engine::Scene::Scene scene;
    auto* root = scene.AddObject("Body");
    auto* spineA = scene.AddObject("Spine A");
    auto* spineB = scene.AddObject("Spine B");
    auto* head = scene.AddObject("Head");
    auto* arm = scene.AddObject("Arm");
    auto* hand = scene.AddObject("Hand");
    auto* headTarget = scene.AddObject("Head target");
    auto* handTarget = scene.AddObject("Hand target");
    using Placement = Engine::Scene::Scene::ObjectPlacement;
    CHECK(scene.MoveObject(spineA, root, Placement::AsChild));
    CHECK(scene.MoveObject(spineB, spineA, Placement::AsChild));
    CHECK(scene.MoveObject(head, spineB, Placement::AsChild));
    CHECK(scene.MoveObject(arm, spineA, Placement::AsChild));
    CHECK(scene.MoveObject(hand, arm, Placement::AsChild));
    spineA->transform.position = {0.f, 1.f, 0.f};
    spineB->transform.position = {0.f, 1.f, 0.f};
    head->transform.position = {0.f, 1.f, 0.f};
    arm->transform.position = {1.f, 0.f, 0.f};
    hand->transform.position = {1.f, 0.f, 0.f};
    headTarget->transform.position = {.5f, 2.6f, 0.f};
    handTarget->transform.position = {1.8f, 1.7f, 0.f};
    auto* headIK = root->AddComponent<Engine::Components::ChainIK>();
    headIK->tipReference = Engine::Core::CaptureComponentReference(
        &head->transform, "Transform");
    headIK->targetReference = Engine::Core::CaptureComponentReference(
        &headTarget->transform, "Transform");
    headIK->iterations = 64;
    auto* handIK = root->AddComponent<Engine::Components::ChainIK>();
    handIK->tipReference = Engine::Core::CaptureComponentReference(
        &hand->transform, "Transform");
    handIK->targetReference = Engine::Core::CaptureComponentReference(
        &handTarget->transform, "Transform");
    handIK->iterations = 64;
    handIK->rootTranslationWeight = .25f;
    handIK->alignToGroundNormal = true;
    scene.ApplyPoseConstraints();
    CHECK(glm::distance(head->transform.GetWorldPosition(),
        headTarget->transform.GetWorldPosition()) < .08f);
    CHECK(glm::distance(hand->transform.GetWorldPosition(),
        handTarget->transform.GetWorldPosition()) < .08f);
    CHECK(std::abs(glm::distance(spineA->transform.GetWorldPosition(),
        spineB->transform.GetWorldPosition()) - 1.f) < 1e-3f);
    const auto solvedHead = head->transform.GetWorldPosition();
    const auto solvedHand = hand->transform.GetWorldPosition();
    scene.ApplyPoseConstraints();
    CHECK(glm::distance(solvedHead, head->transform.GetWorldPosition()) < .02f);
    CHECK(glm::distance(solvedHand, hand->transform.GetWorldPosition()) < .02f);
    Engine::Components::ChainIK restored;
    restored.Deserialize(handIK->Serialize());
    CHECK(restored.tipReference.IsAssigned());
    CHECK(std::abs(restored.rootTranslationWeight - .25f) < 1e-5f);
    CHECK(restored.alignToGroundNormal);
    Engine::Components::ChainIK::Effector unreachable;
    unreachable.chain = {root, spineA, spineB, head};
    unreachable.target = {100.f, 0.f, 0.f};
    CHECK(Engine::Components::ChainIK::SolveMultiple({unreachable}, 32));
    const auto limited = head->transform.GetWorldPosition();
    CHECK(std::isfinite(limited.x) && std::isfinite(limited.y) &&
        std::isfinite(limited.z));
    CHECK(glm::distance(limited, root->transform.GetWorldPosition()) < 3.01f);
    Engine::Scene::Scene terrain;
    auto* hip = terrain.AddObject("Hip");
    auto* knee = terrain.AddObject("Knee");
    auto* foot = terrain.AddObject("Foot");
    auto* floor = terrain.AddObject("Floor");
    auto* footTarget = terrain.AddObject("Foot target");
    CHECK(terrain.MoveObject(knee, hip, Placement::AsChild));
    CHECK(terrain.MoveObject(foot, knee, Placement::AsChild));
    hip->transform.position = {0.f, 1.f, 0.f};
    knee->transform.position = {1.f, 0.f, 0.f};
    foot->transform.position = {1.f, 0.f, 0.f};
    floor->transform.position = {0.f, -.5f, 0.f};
    floor->transform.scale = {10.f, 1.f, 10.f};
    footTarget->transform.position = {1.2f, 1.f, 0.f};
    auto* floorBody = floor->AddComponent<Engine::Components::RigidBody>();
    floorBody->bodyType = "Static";
    floor->AddComponent<Engine::Components::PrimitiveObjectCollider>();
    auto* grounded = hip->AddComponent<Engine::Components::ChainIK>();
    grounded->tipReference = Engine::Core::CaptureComponentReference(
        &foot->transform, "Transform");
    grounded->targetReference = Engine::Core::CaptureComponentReference(
        &footTarget->transform, "Transform");
    grounded->projectTargetToGround = true;
    grounded->groundClearance = .1f;
    terrain.GetPhysics().Step(1.0 / 60.0);
    terrain.ApplyPoseConstraints();
    CHECK(std::abs(foot->transform.GetWorldPosition().y - .1f) < .08f);
    return 0;
}
