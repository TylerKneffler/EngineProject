#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Mesh.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
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

bool MatricesAlmostEqual(const glm::mat4& a, const glm::mat4& b, float epsilon)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::abs(a[column][row] - b[column][row]) > epsilon)
                return false;
        }
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

void WriteVec3(std::ostream& output, const glm::vec3& value)
{
    output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

void WriteMeshState(std::ostream& output, const Engine::Components::Mesh& mesh,
    const std::vector<Engine::Model::Vertex>& baseVertices)
{
    output << "{\"vertexCount\":" << mesh.GetVertexCount()
        << ",\"configurationRevision\":" << mesh.GetConfigurationRevision()
        << ",\"deformed\":"
        << (VerticesChanged(baseVertices, mesh.GetVertices(), 0.0001f)
            ? "true" : "false")
        << ",\"boundsMin\":";
    WriteVec3(output, mesh.GetBoundsMin());
    output << ",\"boundsMax\":";
    WriteVec3(output, mesh.GetBoundsMax());
    output << '}';
}
}

int main(int argc, char** argv)
{
    assert(argc >= 2);
    const std::string meshPath = argv[1];
    const char* snapshotPath = argc >= 3 ? argv[2] : nullptr;

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

    // This coincident portal deliberately competes for the same swept body.
    // Priority, rather than object insertion/update order, must select it.
    Engine::Core::Object* priorityPortalObject = scene.AddObject("PriorityPortal");
    auto* priorityManipulator = priorityPortalObject->AddComponent<SpatialManipulator>();
    priorityManipulator->connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    priorityManipulator->portalTraversalPriority = 100;
    Engine::Core::Object* priorityTargetObject = scene.AddObject("PriorityTargetPortal");
    priorityTargetObject->transform.position = glm::vec3(20.f, 0.f, 0.f);
    auto* priorityTarget = priorityTargetObject->AddComponent<SpatialManipulator>();
    priorityTarget->connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    priorityTarget->portalTraversalPriority = 100;
    priorityManipulator->ConnectToTarget(priorityTarget);

    Engine::Core::Object* traverser = scene.AddObject("Traverser");
    traverser->transform.position = glm::vec3(0.f, 0.f, -3.f);
    traverser->transform.rotation = glm::vec3(0.f, 0.25f, 0.f);
    // The authored cube is 1 unit wide. Keep the successful-traversal
    // fixture inside the 1-unit aperture after its solid rim is applied;
    // the separate blocked sphere below covers the too-large case.
    traverser->transform.scale = glm::vec3(0.4f);

    auto* traverserMesh = traverser->AddComponent<Mesh>();
    traverserMesh->LoadFromFile(meshPath);
    const std::vector<Engine::Model::Vertex> baseVertices = traverserMesh->GetVertices();

    auto* traverserBody = traverser->AddComponent<RigidBody>();
    traverserBody->bodyType = "Dynamic";
    traverserBody->useGravity = false;
    traverserBody->linearDamping = 0.f;
    traverserBody->angularDamping = 0.f;
    traverserBody->collisionMask = ~2;
    traverserBody->freezeRotationX = true;
    traverserBody->freezeRotationY = true;
    traverserBody->freezeRotationZ = true;
    traverserBody->initialLinearVelocity = glm::vec3(0.f, 0.f, 5.f);

    auto* traverserCollider = traverser->AddComponent<PrimitiveObjectCollider>();
    traverserCollider->shape = "Sphere";
    traverserCollider->radius = 0.25f;

    Engine::Core::Object* reverseTraverser = scene.AddObject("ReverseTraverser");
    // This body crosses from the opposite side through the actual 1x1
    // aperture. A separate stationary body below stays inside the broad
    // trigger but outside the aperture to guard the narrow-phase rule.
    reverseTraverser->transform.position = glm::vec3(0.f, 0.2f, 3.f);
    reverseTraverser->transform.scale = glm::vec3(0.4f);
    // Exercise reverse traversal after the forward body has left the shared
    // aperture. Two dynamic cut pieces legitimately collide at one opening;
    // this fixture is testing directionality, not simultaneous-body routing.
    reverseTraverser->enabled = false;
    auto* reverseTraverserMesh = reverseTraverser->AddComponent<Mesh>();
    reverseTraverserMesh->LoadFromFile(meshPath);
    const std::vector<Engine::Model::Vertex> reverseBaseVertices =
        reverseTraverserMesh->GetVertices();
    auto* reverseBody = reverseTraverser->AddComponent<RigidBody>();
    reverseBody->bodyType = "Dynamic";
    reverseBody->useGravity = false;
    reverseBody->linearDamping = 0.f;
    reverseBody->angularDamping = 0.f;
    reverseBody->collisionMask = ~2;
    reverseBody->initialLinearVelocity = glm::vec3(0.f, 0.f, -5.f);
    reverseBody->initialAngularVelocity = glm::vec3(1.f, 2.f, 3.f);
    auto* reverseCollider = reverseTraverser->AddComponent<PrimitiveObjectCollider>();
    reverseCollider->shape = "Sphere";
    reverseCollider->radius = 0.2f;

    // This target-space trigger is never eligible for portal traversal. It
    // verifies that the hidden remote mesh instance participates in Bullet
    // contact generation while the visible mesh is split.
    Engine::Core::Object* remoteCollisionProbe = scene.AddObject("RemoteCollisionProbe");
    remoteCollisionProbe->transform.position = glm::vec3(20.f, 0.f, 0.f);
    auto* remoteProbeBody = remoteCollisionProbe->AddComponent<RigidBody>();
    remoteProbeBody->bodyType = "Dynamic";
    remoteProbeBody->isTrigger = true;
    remoteProbeBody->useGravity = false;
    remoteProbeBody->collisionMask = ~2;
    auto* remoteProbeCollider =
        remoteCollisionProbe->AddComponent<PrimitiveObjectCollider>();
    remoteProbeCollider->shape = "Sphere";
    remoteProbeCollider->radius = 0.15f;

    // Starting inside the trigger on one side is not a traversal. This guards
    // against the old side-based behavior, which teleported immediately.
    Engine::Core::Object* stationaryTraverser = scene.AddObject("StationaryTraverser");
    stationaryTraverser->transform.position = glm::vec3(0.f, -0.6f, 0.5f);
    auto* stationaryBody = stationaryTraverser->AddComponent<RigidBody>();
    stationaryBody->bodyType = "Dynamic";
    stationaryBody->useGravity = false;
    stationaryBody->collisionMask = ~2;
    auto* stationaryCollider = stationaryTraverser->AddComponent<PrimitiveObjectCollider>();
    stationaryCollider->shape = "Sphere";
    stationaryCollider->radius = 0.2f;

    // Its centre follows the aperture centreline, but its sphere is wider
    // than the opening. The solid portal rim must prevent this body from
    // crossing; centre-point-only portal logic would incorrectly teleport it.
    Engine::Core::Object* blockedTraverser = scene.AddObject("BlockedTraverser");
    blockedTraverser->enabled = false;
    blockedTraverser->transform.position = glm::vec3(0.f, 0.f, -3.f);
    auto* blockedBody = blockedTraverser->AddComponent<RigidBody>();
    blockedBody->bodyType = "Dynamic";
    blockedBody->useGravity = false;
    blockedBody->linearDamping = 0.f;
    blockedBody->collisionLayer = 2;
    blockedBody->initialLinearVelocity = glm::vec3(0.f, 0.f, 5.f);
    auto* blockedCollider = blockedTraverser->AddComponent<PrimitiveObjectCollider>();
    blockedCollider->shape = "Sphere";
    blockedCollider->radius = 0.75f;

    // Its centre is inside the aperture, but its right-hand bounds overlap
    // the rim. This must collide and must not traverse on a centre-point ray.
    Engine::Core::Object* edgeBlockedTraverser = scene.AddObject(
        "EdgeBlockedTraverser");
    edgeBlockedTraverser->enabled = false;
    edgeBlockedTraverser->transform.position = glm::vec3(0.35f, 0.f, -3.f);
    edgeBlockedTraverser->transform.scale = glm::vec3(0.4f);
    auto* edgeBlockedMesh = edgeBlockedTraverser->AddComponent<Mesh>();
    edgeBlockedMesh->LoadFromFile(meshPath);
    auto* edgeBlockedBody = edgeBlockedTraverser->AddComponent<RigidBody>();
    edgeBlockedBody->bodyType = "Dynamic";
    edgeBlockedBody->useGravity = false;
    edgeBlockedBody->linearDamping = 0.f;
    edgeBlockedBody->collisionLayer = 2;
    edgeBlockedBody->initialLinearVelocity = glm::vec3(0.f, 0.f, 5.f);
    auto* edgeBlockedCollider =
        edgeBlockedTraverser->AddComponent<PrimitiveObjectCollider>();
    edgeBlockedCollider->shape = "Box";
    edgeBlockedCollider->size = glm::vec3(1.f);

    // A second portal is intentionally disconnected while its mesh is split.
    // The disconnect must leave the local cut in place and materialize the
    // remote cut as an independent mesh/collider object.
    Engine::Core::Object* breakPortalObject = scene.AddObject("BreakPortal");
    breakPortalObject->transform.position = glm::vec3(40.f, 0.f, 0.f);
    auto* breakPortalBody = breakPortalObject->AddComponent<RigidBody>();
    breakPortalBody->bodyType = "Static";
    breakPortalBody->isTrigger = true;
    breakPortalBody->useGravity = false;
    auto* breakPortalCollider = breakPortalObject->AddComponent<PrimitiveObjectCollider>();
    breakPortalCollider->shape = "Box";
    breakPortalCollider->size = glm::vec3(4.f, 4.f, 2.f);
    auto* breakManipulator = breakPortalObject->AddComponent<SpatialManipulator>();
    breakManipulator->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    Engine::Core::Object* breakTargetObject = scene.AddObject("BreakTargetPortal");
    breakTargetObject->transform.position = glm::vec3(60.f, 0.f, 0.f);
    auto* breakTarget = breakTargetObject->AddComponent<SpatialManipulator>();
    breakTarget->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    breakManipulator->ConnectToTarget(breakTarget);

    Engine::Core::Object* breakTraverser = scene.AddObject("BreakTraverser");
    breakTraverser->transform.position = glm::vec3(40.f, 0.f, -3.f);
    breakTraverser->transform.scale = glm::vec3(0.4f);
    auto* breakMesh = breakTraverser->AddComponent<Mesh>();
    breakMesh->LoadFromFile(meshPath);
    auto* breakBody = breakTraverser->AddComponent<RigidBody>();
    breakBody->bodyType = "Dynamic";
    breakBody->useGravity = false;
    breakBody->linearDamping = 0.f;
    breakBody->collisionMask = ~2;
    breakBody->initialLinearVelocity = glm::vec3(0.f, 0.f, 5.f);
    auto* breakCollider = breakTraverser->AddComponent<PrimitiveObjectCollider>();
    breakCollider->shape = "Sphere";
    breakCollider->radius = 0.2f;

    bool sawBeginOverlap = false;
    bool sawEndOverlap = false;
    bool sawTeleport = false;
    bool sawPriorityTeleport = false;
    bool sawReverseTeleport = false;
    bool stationaryTeleported = false;
    bool sawCpuMeshMutation = false;
    bool sawGpuSplitRenderInstances = false;
    bool sawDistinctTraversalCharts = false;
    bool sawContinuousSplitBeforeTeleport = false;
    bool sawContinuousSplitAfterTeleport = false;
    bool sawPortalMeshCollider = false;
    bool sawPortalLocalMeshCollider = false;
    bool sawRemoteMeshColliderContact = false;
    bool blockedTraverserTeleported = false;
    bool edgeBlockedTraverserTeleported = false;
    bool edgeBlockedTraverserHitRim = false;
    bool splitMaterialized = false;
    Engine::Core::Object* materializedFragment = nullptr;
    glm::vec3 velocityAfterTeleport(0.f);
    glm::vec3 angularVelocityAfterReverseTeleport(0.f);
    glm::mat3 orientationAfterTeleport(1.f);

    std::ofstream snapshot;
    bool firstSnapshot = true;
    if (snapshotPath)
    {
        snapshot.open(snapshotPath, std::ios::trunc);
        assert(snapshot.good());
        snapshot << "{\n  \"fixture\": \"portal_trigger_traversal\",\n"
            << "  \"collisionFixture\": \"portal trigger plus primitive body colliders\",\n"
            << "  \"frames\": [\n";
    }
    const auto captureSnapshot = [&](int frame, const char* reason)
    {
        if (!snapshot.is_open())
            return;
        if (!firstSnapshot)
            snapshot << ",\n";
        firstSnapshot = false;
        snapshot << "    {\"frame\":" << frame << ",\"reason\":\""
            << reason << "\",\"sourceTrigger\":{\"forwardOverlap\":"
            << (sourcePortalBody->IsOverlapping(traverserBody) ? "true" : "false")
            << ",\"reverseOverlap\":"
            << (sourcePortalBody->IsOverlapping(reverseBody) ? "true" : "false")
            << ",\"remoteMeshColliderContact\":"
            << (remoteProbeBody->IsColliding() ? "true" : "false")
            << "},\"forward\":{\"portalMeshColliderInstances\":"
            << scene.GetPhysics().GetPortalMeshColliderCount(*traverserBody)
            << ",\"localPortalMeshCollider\":"
            << (traverserBody->HasPortalLocalMeshCollider() ? "true" : "false")
            << ",\"position\":";
        WriteVec3(snapshot, traverser->transform.GetWorldPosition());
        snapshot << ",\"linearVelocity\":";
        WriteVec3(snapshot, traverserBody->GetLinearVelocity());
        snapshot << ",\"angularVelocity\":";
        WriteVec3(snapshot, traverserBody->GetAngularVelocity());
        snapshot << ",\"mesh\":";
        WriteMeshState(snapshot, *traverserMesh, baseVertices);
        snapshot << "},\"reverse\":{\"portalMeshColliderInstances\":"
            << scene.GetPhysics().GetPortalMeshColliderCount(*reverseBody)
            << ",\"localPortalMeshCollider\":"
            << (reverseBody->HasPortalLocalMeshCollider() ? "true" : "false")
            << ",\"position\":";
        WriteVec3(snapshot, reverseTraverser->transform.GetWorldPosition());
        snapshot << ",\"linearVelocity\":";
        WriteVec3(snapshot, reverseBody->GetLinearVelocity());
        snapshot << ",\"angularVelocity\":";
        WriteVec3(snapshot, reverseBody->GetAngularVelocity());
        snapshot << ",\"mesh\":";
        WriteMeshState(snapshot, *reverseTraverserMesh, reverseBaseVertices);
        snapshot << "},\"apertureGuards\":{\"oversizedTeleported\":"
            << (blockedTraverserTeleported ? "true" : "false")
            << ",\"edgeCubeTeleported\":"
            << (edgeBlockedTraverserTeleported ? "true" : "false")
            << ",\"edgeCubeHitRim\":"
            << (edgeBlockedTraverserHitRim ? "true" : "false")
            << "},\"disconnect\":{\"splitMaterialized\":"
            << (splitMaterialized ? "true" : "false")
            << ",\"fragmentPresent\":"
            << (materializedFragment ? "true" : "false") << "}}";
    };

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
    captureSnapshot(-1, "initial");
    bool previousForwardDeformed = false;
    bool previousReverseDeformed = false;
    for (int frame = 0; frame < 240; ++frame)
    {
        if (frame == 100)
            reverseTraverser->enabled = true;
        if (frame == 120)
            blockedTraverser->enabled = true;
        if (frame == 170)
            edgeBlockedTraverser->enabled = true;
        scene.Update(1.f / 60.f);

        const bool beganForwardOverlap = sourcePortalBody->DidBeginOverlap(traverserBody);
        const bool endedForwardOverlap = sourcePortalBody->DidEndOverlap(traverserBody);
        if (beganForwardOverlap)
            sawBeginOverlap = true;
        if (endedForwardOverlap)
            sawEndOverlap = true;

        const glm::vec3 worldPosition = traverser->transform.GetWorldPosition();
        bool teleportedThisFrame = false;
        bool reverseTeleportedThisFrame = false;
        if (worldPosition.x > 6.f && !sawTeleport)
        {
            sawTeleport = true;
            teleportedThisFrame = true;
            velocityAfterTeleport = traverserBody->GetLinearVelocity();
            orientationAfterTeleport = RotationFrom(
                traverser->transform.GetWorldMatrix());
        }
        if (worldPosition.x > 16.f)
            sawPriorityTeleport = true;

        if (reverseTraverser->transform.GetWorldPosition().x > 6.f &&
            !sawReverseTeleport)
        {
            sawReverseTeleport = true;
            reverseTeleportedThisFrame = true;
            angularVelocityAfterReverseTeleport = reverseBody->GetAngularVelocity();
        }
        if (stationaryTraverser->transform.GetWorldPosition().x > 6.f)
            stationaryTeleported = true;
        if (blockedTraverser->transform.GetWorldPosition().x > 6.f)
            blockedTraverserTeleported = true;
        if (edgeBlockedTraverser->transform.GetWorldPosition().x > 6.f)
            edgeBlockedTraverserTeleported = true;
        edgeBlockedTraverserHitRim = edgeBlockedTraverserHitRim ||
            edgeBlockedBody->IsColliding();

        if (!splitMaterialized && breakBody->HasPortalLocalMeshCollider())
        {
            const size_t objectCount = scene.GetObjects().size();
            breakManipulator->Disconnect();
            splitMaterialized = scene.GetObjects().size() == objectCount + 1u;
            for (const auto& object : scene.GetObjects())
            {
                if (object && object->name == "BreakTraverser (Portal Fragment)")
                {
                    materializedFragment = object.get();
                    break;
                }
            }
        }

        const bool forwardDeformed = VerticesChanged(baseVertices,
            traverserMesh->GetVertices(), 0.0001f);
        const bool reverseDeformed = VerticesChanged(reverseBaseVertices,
            reverseTraverserMesh->GetVertices(), 0.0001f);
        sawCpuMeshMutation = sawCpuMeshMutation || forwardDeformed ||
            reverseDeformed;
        std::vector<SpatialManipulator::TraversalRenderInstance>
            traversalRenderInstances;
        sourceManipulator->AppendTraversalRenderInstances(
            traversalRenderInstances);
        targetManipulator->AppendTraversalRenderInstances(
            traversalRenderInstances);
        priorityManipulator->AppendTraversalRenderInstances(
            traversalRenderInstances);
        priorityTarget->AppendTraversalRenderInstances(
            traversalRenderInstances);
        bool foundLocalRenderInstance = false;
        bool foundRemoteRenderInstance = false;
        const SpatialManipulator* localChart = nullptr;
        const SpatialManipulator* remoteChart = nullptr;
        const SpatialManipulator::TraversalRenderInstance* localInstance = nullptr;
        const SpatialManipulator::TraversalRenderInstance* remoteInstance = nullptr;
        for (const auto& instance : traversalRenderInstances)
        {
            if (instance.object != traverser && instance.object != reverseTraverser)
                continue;
            foundLocalRenderInstance = foundLocalRenderInstance || !instance.remote;
            foundRemoteRenderInstance = foundRemoteRenderInstance || instance.remote;
            if (instance.object == traverser)
            {
                if (instance.remote)
                {
                    remoteChart = instance.chartPortal;
                    remoteInstance = &instance;
                }
                else
                {
                    localChart = instance.chartPortal;
                    localInstance = &instance;
                }
            }
        }
        sawGpuSplitRenderInstances = sawGpuSplitRenderInstances ||
            (foundLocalRenderInstance && foundRemoteRenderInstance);
        sawDistinctTraversalCharts = sawDistinctTraversalCharts ||
            (localChart && remoteChart && localChart != remoteChart);
        if (localInstance && remoteInstance && localChart && remoteChart &&
            MatricesAlmostEqual(remoteInstance->world,
                localChart->GetRenderPortalWorldTransformTo(*remoteChart) *
                    localInstance->world,
                0.001f))
        {
            if (sawTeleport)
                sawContinuousSplitAfterTeleport = true;
            else
                sawContinuousSplitBeforeTeleport = true;
        }
        if (scene.GetPhysics().GetPortalMeshColliderCount(*traverserBody) > 0u ||
            scene.GetPhysics().GetPortalMeshColliderCount(*reverseBody) > 0u)
        {
            sawPortalMeshCollider = true;
        }
        sawPortalLocalMeshCollider = sawPortalLocalMeshCollider ||
            traverserBody->HasPortalLocalMeshCollider() ||
            reverseBody->HasPortalLocalMeshCollider();
        sawRemoteMeshColliderContact = sawRemoteMeshColliderContact ||
            remoteProbeBody->IsColliding();
        if (frame % 15 == 0 || beganForwardOverlap || endedForwardOverlap ||
            forwardDeformed != previousForwardDeformed ||
            reverseDeformed != previousReverseDeformed || teleportedThisFrame ||
            reverseTeleportedThisFrame)
        {
            captureSnapshot(frame, "simulation");
        }
        previousForwardDeformed = forwardDeformed;
        previousReverseDeformed = reverseDeformed;
    }

    const std::vector<Engine::Model::Vertex> finalVertices = traverserMesh->GetVertices();
    const std::vector<Engine::Model::Vertex> finalReverseVertices =
        reverseTraverserMesh->GetVertices();
    const glm::vec3 finalTraverserPosition = traverser->transform.GetWorldPosition();
    const glm::vec3 finalReversePosition = reverseTraverser->transform.GetWorldPosition();

    assert(sawBeginOverlap);
    assert(sawEndOverlap);
    assert(sawTeleport);
    assert(sawPriorityTeleport);
    assert(sawReverseTeleport);
    assert(!stationaryTeleported);
    assert(!blockedTraverserTeleported);
    assert(!edgeBlockedTraverserTeleported);
    assert(edgeBlockedTraverserHitRim);
    assert(splitMaterialized);
    assert(materializedFragment);
    assert(materializedFragment->GetComponent<Mesh>());
    assert(materializedFragment->GetComponent<MeshObjectCollider>());
    assert(materializedFragment->GetComponent<RigidBody>());
    assert(breakMesh->GetVertexCount() > 0u);
    assert(materializedFragment->GetComponent<Mesh>()->GetVertexCount() > 0u);
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
    // Traversal now keeps the authored mesh buffer stable and submits local /
    // remote chart instances using GPU clip planes. This exercises both travel
    // directions without per-frame CPU mesh uploads.
    assert(!sawCpuMeshMutation);
    assert(sawGpuSplitRenderInstances);
    assert(sawDistinctTraversalCharts);
    assert(sawContinuousSplitBeforeTeleport);
    assert(sawContinuousSplitAfterTeleport);
    assert(sawPortalMeshCollider);
    assert(sawPortalLocalMeshCollider);
    assert(sawRemoteMeshColliderContact);
    assert(scene.GetPhysics().GetPortalMeshColliderCount(*traverserBody) == 0u);
    assert(scene.GetPhysics().GetPortalMeshColliderCount(*reverseBody) == 0u);
    assert(!traverserBody->HasPortalLocalMeshCollider());
    assert(!reverseBody->HasPortalLocalMeshCollider());
    assert(VerticesAlmostEqual(baseVertices, finalVertices, 0.02f));
    assert(VerticesAlmostEqual(reverseBaseVertices, finalReverseVertices, 0.02f));

    captureSnapshot(240, "final");
    if (snapshot.is_open())
        snapshot << "\n  ]\n}\n";

    return 0;
}
