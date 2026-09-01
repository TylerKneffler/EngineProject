#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Mesh.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "Assertion failed: %s, line %d\\n", #condition, __LINE__); \
    return 1; } } while (false)

namespace
{
bool VerticesChanged(const std::vector<Engine::Model::Vertex>& a,
    const std::vector<Engine::Model::Vertex>& b, float epsilon)
{
    if (a.size() != b.size())
        return true;

    for (size_t i = 0; i < a.size(); ++i)
    {
        const float dx = std::abs(a[i].pos[0] - b[i].pos[0]);
        const float dy = std::abs(a[i].pos[1] - b[i].pos[1]);
        const float dz = std::abs(a[i].pos[2] - b[i].pos[2]);
        if (dx > epsilon || dy > epsilon || dz > epsilon)
            return true;
    }
    return false;
}

bool VerticesAlmostEqual(const std::vector<Engine::Model::Vertex>& a,
    const std::vector<Engine::Model::Vertex>& b, float epsilon)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        const float dx = std::abs(a[i].pos[0] - b[i].pos[0]);
        const float dy = std::abs(a[i].pos[1] - b[i].pos[1]);
        const float dz = std::abs(a[i].pos[2] - b[i].pos[2]);
        if (dx > epsilon || dy > epsilon || dz > epsilon)
            return false;
    }
    return true;
}

glm::mat3 RotationFrom(const glm::mat4& transform)
{
    glm::mat3 rotation(1.f);
    for (int column = 0; column < 3; ++column)
        rotation[column] = glm::normalize(glm::vec3(transform[column]));
    return rotation;
}
}

int main(int argc, char** argv)
{
    assert(argc >= 2);
    const std::string meshPath = argv[1];

    using namespace Engine::Scene;
    using namespace Engine::Components;

    Scene scene;

    Engine::Core::Object* sourcePortalObject = scene.AddObject("SourcePortal");
    sourcePortalObject->transform.position = glm::vec3(0.f, 0.f, 0.f);

    auto* sourcePortalBody = sourcePortalObject->AddComponent<RigidBody>();
    sourcePortalBody->bodyType = "Static";
    sourcePortalBody->isTrigger = true;
    sourcePortalBody->useGravity = false;

    auto* sourcePortalCollider = sourcePortalObject->AddComponent<PrimitiveObjectCollider>();
    sourcePortalCollider->shape = "Box";
    sourcePortalCollider->size = glm::vec3(4.f, 4.f, 2.f);

    auto* sourceManipulator = sourcePortalObject->AddComponent<SpatialManipulator>();
    sourceManipulator->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    sourceManipulator->portalPoint = glm::vec3(0.f, 0.f, 0.f);
    sourceManipulator->portalNormal = glm::vec3(0.f, 0.f, 1.f);
    sourceManipulator->traversalBlendDistance = 1.25f;
    sourceManipulator->deformationStrength = 1.f;

    Engine::Core::Object* targetPortalObject = scene.AddObject("TargetPortal");
    targetPortalObject->transform.position = glm::vec3(10.f, 0.f, 0.f);

    auto* targetManipulator = targetPortalObject->AddComponent<SpatialManipulator>();
    targetManipulator->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    targetManipulator->portalPoint = glm::vec3(0.f, 0.f, 0.f);
    targetManipulator->portalNormal = glm::vec3(0.f, 0.f, 1.f);

    sourceManipulator->portalPointCount = 4;
    sourceManipulator->portalShapePoint0 = glm::vec3(-0.5f, -0.5f, 0.f);
    sourceManipulator->portalShapePoint1 = glm::vec3(0.5f, -0.5f, 0.f);
    sourceManipulator->portalShapePoint2 = glm::vec3(0.5f, 0.5f, 0.f);
    sourceManipulator->portalShapePoint3 = glm::vec3(-0.5f, 0.5f, 0.f);

    targetManipulator->portalPointCount = 4;
    targetManipulator->portalShapePoint0 = glm::vec3(-1.0f, -1.0f, 0.f);
    targetManipulator->portalShapePoint1 = glm::vec3(1.0f, -1.0f, 0.f);
    targetManipulator->portalShapePoint2 = glm::vec3(1.0f, 1.0f, 0.f);
    targetManipulator->portalShapePoint3 = glm::vec3(-1.0f, 1.0f, 0.f);

    sourceManipulator->ConnectToTarget(targetManipulator);

    Engine::Core::Object* traverser = scene.AddObject("Traverser");
    traverser->transform.position = glm::vec3(0.f, 0.f, -3.f);
    traverser->transform.rotation = glm::vec3(0.f, 0.25f, 0.f);

    auto* traverserMesh = traverser->AddComponent<Mesh>();
    traverserMesh->LoadFromFile(meshPath);
    const std::vector<Engine::Model::Vertex> baseVertices = traverserMesh->GetVertices();

    auto* traverserBody = traverser->AddComponent<RigidBody>();
    traverserBody->bodyType = "Dynamic";
    traverserBody->useGravity = false;
    traverserBody->linearDamping = 0.f;
    traverserBody->angularDamping = 0.f;
    traverserBody->initialLinearVelocity = glm::vec3(0.f, 0.f, 5.f);

    auto* traverserCollider = traverser->AddComponent<PrimitiveObjectCollider>();
    traverserCollider->shape = "Sphere";
    traverserCollider->radius = 0.25f;

    Engine::Core::Object* reverseTraverser = scene.AddObject("ReverseTraverser");
    // This body crosses from the opposite side through the actual 1x1
    // aperture. A separate stationary body below stays inside the broad
    // trigger but outside the aperture to guard the narrow-phase rule.
    reverseTraverser->transform.position = glm::vec3(0.f, 0.4f, 3.f);
    auto* reverseBody = reverseTraverser->AddComponent<RigidBody>();
    reverseBody->bodyType = "Dynamic";
    reverseBody->useGravity = false;
    reverseBody->linearDamping = 0.f;
    reverseBody->angularDamping = 0.f;
    reverseBody->initialLinearVelocity = glm::vec3(0.f, 0.f, -5.f);
    reverseBody->initialAngularVelocity = glm::vec3(1.f, 2.f, 3.f);
    auto* reverseCollider = reverseTraverser->AddComponent<PrimitiveObjectCollider>();
    reverseCollider->shape = "Sphere";
    reverseCollider->radius = 0.2f;

    // Starting inside the trigger on one side is not a traversal. This guards
    // against the old side-based behavior, which teleported immediately.
    Engine::Core::Object* stationaryTraverser = scene.AddObject("StationaryTraverser");
    stationaryTraverser->transform.position = glm::vec3(0.f, -0.6f, 0.5f);
    auto* stationaryBody = stationaryTraverser->AddComponent<RigidBody>();
    stationaryBody->bodyType = "Dynamic";
    stationaryBody->useGravity = false;
    auto* stationaryCollider = stationaryTraverser->AddComponent<PrimitiveObjectCollider>();
    stationaryCollider->shape = "Sphere";
    stationaryCollider->radius = 0.2f;

    bool sawBeginOverlap = false;
    bool sawEndOverlap = false;
    bool sawTeleport = false;
    bool sawReverseTeleport = false;
    bool stationaryTeleported = false;
    bool sawMeshDeformation = false;
    glm::vec3 velocityAfterTeleport(0.f);
    glm::vec3 angularVelocityAfterReverseTeleport(0.f);
    glm::mat3 orientationAfterTeleport(1.f);

    const glm::mat4 portalTransform =
        sourceManipulator->GetPortalWorldTransformTo(*targetManipulator);
    const glm::mat3 portalLinear(portalTransform);
    const glm::mat3 portalRotation = RotationFrom(portalTransform);
    const glm::mat3 expectedOrientation = portalRotation *
        RotationFrom(traverser->transform.GetWorldMatrix());
    const glm::vec3 expectedVelocity = portalLinear *
        traverserBody->initialLinearVelocity;
    const glm::vec3 expectedReverseAngularVelocity = portalRotation *
        reverseBody->initialAngularVelocity;

    scene.Start();
    for (int frame = 0; frame < 240; ++frame)
    {
        scene.Update(1.f / 60.f);

        if (sourcePortalBody->DidBeginOverlap(traverserBody))
            sawBeginOverlap = true;
        if (sourcePortalBody->DidEndOverlap(traverserBody))
            sawEndOverlap = true;

        const glm::vec3 worldPosition = traverser->transform.GetWorldPosition();
        if (worldPosition.x > 6.f && !sawTeleport)
        {
            sawTeleport = true;
            velocityAfterTeleport = traverserBody->GetLinearVelocity();
            orientationAfterTeleport = RotationFrom(
                traverser->transform.GetWorldMatrix());
        }

        if (reverseTraverser->transform.GetWorldPosition().x > 6.f &&
            !sawReverseTeleport)
        {
            sawReverseTeleport = true;
            angularVelocityAfterReverseTeleport = reverseBody->GetAngularVelocity();
        }
        if (stationaryTraverser->transform.GetWorldPosition().x > 6.f)
            stationaryTeleported = true;

        if (VerticesChanged(baseVertices, traverserMesh->GetVertices(), 0.0001f))
            sawMeshDeformation = true;
    }

    const std::vector<Engine::Model::Vertex> finalVertices = traverserMesh->GetVertices();
    const glm::vec3 finalTraverserPosition = traverser->transform.GetWorldPosition();
    const glm::vec3 finalReversePosition = reverseTraverser->transform.GetWorldPosition();

    assert(sawBeginOverlap);
    assert(sawEndOverlap);
    assert(sawTeleport);
    assert(sawReverseTeleport);
    assert(!stationaryTeleported);
    // A stale source-trigger overlap after SetWorldPose must not map either
    // traverser back through the portal a second time.
    assert(finalTraverserPosition.x > 6.f);
    assert(finalReversePosition.x > 6.f);
    // The portal must preserve the transformed travel direction. Contacts in
    // the destination frame may legitimately adjust the exact magnitude on
    // the following simulation sample.
    assert(glm::dot(velocityAfterTeleport, expectedVelocity) > 0.01f);
    assert(glm::dot(angularVelocityAfterReverseTeleport,
        expectedReverseAngularVelocity) > 0.01f);
    for (int column = 0; column < 3; ++column)
        assert(glm::dot(orientationAfterTeleport[column],
            expectedOrientation[column]) > 0.f);
    assert(sawMeshDeformation);
    assert(VerticesAlmostEqual(baseVertices, finalVertices, 0.02f));

    return 0;
}
