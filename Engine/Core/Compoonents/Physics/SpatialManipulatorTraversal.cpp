#include "SpatialManipulator.h"

#include "Core/Object.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <unordered_set>

namespace Engine::Components
{
namespace
{
float Clamp01(float value)
{
    return std::max(0.f, std::min(1.f, value));
}

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared <= 1e-8f ? fallback :
        value / std::sqrt(lengthSquared);
}

bool MatricesNearlyEqual(const glm::mat4& first, const glm::mat4& second,
    float epsilon = 1e-5f)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (std::abs(first[column][row] - second[column][row]) > epsilon)
                return false;
        }
    }
    return true;
}
}

RigidBody* SpatialManipulator::ResolveTraversalTriggerBody() const
{
    if (!Owner)
        return nullptr;
    if (traversalTriggerBodyReference.IsAssigned())
        return Engine::Core::ResolveComponentReference<RigidBody>(Owner,
            traversalTriggerBodyReference);
    return Owner->GetComponent<RigidBody>();
}

Mesh* SpatialManipulator::ResolveMeshForObject(Engine::Core::Object* object) const
{
    if (!object)
        return nullptr;
    if (meshReference.IsAssigned())
    {
        if (Mesh* referenced = Engine::Core::ResolveComponentReference<Mesh>(object,
            meshReference))
            return referenced;
    }
    return object->GetComponent<Mesh>();
}

float SpatialManipulator::ComputeSignedDistanceToPortalPlane(
    const glm::vec3& worldPoint) const
{
    if (!Owner)
        return 0.f;
    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 portalWorldPoint = glm::vec3(ownerWorld *
        glm::vec4(portalPoint, 1.f));
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(
        glm::mat3(ownerWorld)));
    const glm::vec3 portalWorldNormal = SafeNormalize(normalMatrix *
        SafeNormalize(portalNormal, glm::vec3(0.f, 0.f, 1.f)),
        glm::vec3(0.f, 0.f, 1.f));
    return glm::dot(worldPoint - portalWorldPoint, portalWorldNormal);
}

void SpatialManipulator::ResetTraversalMeshDeformation(RigidBody* traversingBody)
{
    if (!traversingBody)
        return;

    auto it = m_traversalStates.find(traversingBody);
    if (it == m_traversalStates.end())
        return;

    // A split body owns two independent collision pieces. Remove the remote
    // proxy before rebuilding the local dynamic body: the proxy filter holds
    // the native owner pointer and must never observe the replaced body.
    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().RemovePortalMeshCollider(&it->second);
    traversingBody->ClearPortalLocalMeshCollider(&it->second);

    it->second.meshDeformed = false;
    it->second.hasCollisionCut = false;
    it->second.postTeleportVisual = false;
    it->second.lastMesh = nullptr;
    it->second.baseVertices.clear();
    it->second.localMeshVertices.clear();
    it->second.remoteMeshVertices.clear();
    it->second.remoteRenderMesh.reset();
    it->second.piecewiseWarp = false;
    it->second.localCollisionVertices.clear();
    it->second.localChartPortal = nullptr;
    it->second.remoteChartPortal = nullptr;
}

void SpatialManipulator::ResetTraversalMeshDeformation()
{
    for (const auto& pair : m_traversalStates)
        ResetTraversalMeshDeformation(const_cast<RigidBody*>(pair.first));
    m_traversalStates.clear();
}

void SpatialManipulator::AppendTraversalRenderInstances(
    std::vector<TraversalRenderInstance>& output) const
{
    for (const auto& pair : m_traversalStates)
    {
        const RigidBody* body = pair.first;
        const TraversalState& state = pair.second;
        if (!body || !body->Owner || !state.meshDeformed || !state.lastMesh)
            continue;

        glm::mat4 localWorld = body->Owner->transform.GetWorldMatrixWithLayer();
        if (state.postTeleportVisual)
        {
            // The physical owner is now in the target chart. Reconstruct its
            // source-chart transform for the trailing half; applying the
            // forward transform below then returns the leading half to the
            // physical target chart.
            localWorld = glm::inverse(state.remoteRenderWorldTransform) *
                localWorld;
        }
        output.push_back({ body->Owner, const_cast<Mesh*>(state.lastMesh),
            localWorld, state.localRenderClipPlane, state.localChartPortal,
            false });
        Mesh* remoteMesh = state.piecewiseWarp && state.remoteRenderMesh
            ? state.remoteRenderMesh.get() : const_cast<Mesh*>(state.lastMesh);
        const glm::mat4 remoteWorld = state.piecewiseWarp
            ? localWorld : state.remoteRenderWorldTransform * localWorld;
        output.push_back({ body->Owner, remoteMesh, remoteWorld,
            state.remoteRenderClipPlane, state.remoteChartPortal, true });
    }
}

void SpatialManipulator::ApplyTraversalMeshDeformation(SpatialManipulator* target,
    RigidBody* traversingBody, bool mapPositiveHalf)
{
    if (!deformMeshOnTraversal || !target || !Owner || !target->Owner ||
        !traversingBody || !traversingBody->Owner)
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }
    if (!HasCompatiblePortalShapeWith(*target))
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    Mesh* mesh = ResolveMeshForObject(traversingBody->Owner);
    if (!mesh)
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    const std::vector<Mesh::Vertex>& currentVertices = mesh->GetVertices();
    if (currentVertices.empty())
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    TraversalState& state = m_traversalStates[traversingBody];
    state.localChartPortal = this;
    state.remoteChartPortal = target;
    if (state.lastMesh != mesh)
    {
        if (Owner && Owner->GetScene())
            Owner->GetScene()->GetPhysics().RemovePortalMeshCollider(&state);
        traversingBody->ClearPortalLocalMeshCollider(&state);
        state.lastMesh = mesh;
        state.baseVertices = currentVertices;
        state.meshDeformed = false;
    }
    else if (!state.meshDeformed)
    {
        state.baseVertices = currentVertices;
    }

    const glm::mat4 bodyWorld = traversingBody->Owner->transform.GetWorldMatrix();
    const glm::mat4 bodyWorldInverse = glm::inverse(bodyWorld);
    const glm::mat4 portalWorldTransform = GetPortalWorldTransformTo(*target);
    const bool piecewiseWarp = UsesPiecewisePortalWarpTo(*target);

    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 sourceWorldPortalPoint = glm::vec3(ownerWorld *
        glm::vec4(portalPoint, 1.f));
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(
        glm::mat3(ownerWorld)));
    const glm::vec3 sourceWorldPortalNormal = SafeNormalize(normalMatrix *
        SafeNormalize(portalNormal, glm::vec3(0.f, 0.f, 1.f)),
        glm::vec3(0.f, 0.f, 1.f));

    const glm::mat3 bodyLinear(bodyWorld);
    const glm::vec3 localPlanePoint = glm::vec3(bodyWorldInverse *
        glm::vec4(sourceWorldPortalPoint, 1.f));
    const glm::vec3 localPlaneNormal = SafeNormalize(
        glm::transpose(bodyLinear) * sourceWorldPortalNormal,
        glm::vec3(0.f, 0.f, 1.f));

    // Render the two halves directly from the original mesh buffer. The
    // fragment shader clips each chart in world space, eliminating the old
    // per-frame combined-mesh upload and preserving independent transforms,
    // sort positions, shadow paths, and portal views.
    const glm::mat4 renderPortalWorldTransform =
        GetRenderPortalWorldTransformTo(*target);
    const glm::mat4 renderSourceFrame = GetRenderPortalWorldFrame();
    const glm::vec3 renderSourcePoint(renderSourceFrame[3]);
    // GetRenderPortalWorldFrame already contains an orthonormal world-space
    // basis.  Applying its inverse-transpose to a *world* +Z vector mixes
    // coordinate spaces and produces an unrelated clip normal whenever a
    // portal is rotated.  That made both chart instances disappear or overlap
    // for rotated portals.  Start with the authored world normal, then map the
    // complete plane through the source-to-target transform below.
    const glm::vec3 renderSourceNormal = SafeNormalize(
        glm::vec3(renderSourceFrame[2]), glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 renderRemoteNormal = SafeNormalize(
        glm::transpose(glm::inverse(glm::mat3(renderPortalWorldTransform))) *
            renderSourceNormal,
        glm::vec3(0.f, 0.f, 1.f));
    const glm::vec3 localClipNormal = mapPositiveHalf
        ? -renderSourceNormal : renderSourceNormal;
    const glm::vec3 remoteClipNormal = mapPositiveHalf
        ? renderRemoteNormal : -renderRemoteNormal;
    state.remoteRenderWorldTransform = renderPortalWorldTransform;
    state.localRenderClipPlane = glm::vec4(localClipNormal,
        -glm::dot(localClipNormal, renderSourcePoint));
    const glm::vec3 renderRemotePoint = glm::vec3(
        renderPortalWorldTransform * glm::vec4(renderSourcePoint, 1.f));
    state.remoteRenderClipPlane = glm::vec4(remoteClipNormal,
        -glm::dot(remoteClipNormal, renderRemotePoint));
    state.meshDeformed = true;

    // A moving cut normally changes only a small amount between physics ticks.
    // Keep the local convex hull and remote Bvh proxy alive until the cut has
    // moved a meaningful distance, changed direction, or its portal mapping
    // changes. Rendering remains exact every frame because it is GPU-clipped.
    const float cutUpdateDistance = std::max(0.001f,
        portalCollisionCutUpdateDistance);
    const bool planeMoved = glm::length(localPlanePoint -
        state.lastCollisionPlanePoint) >= cutUpdateDistance ||
        glm::dot(localPlaneNormal, state.lastCollisionPlaneNormal) < 0.9995f;
    const bool needsCollisionCut = !state.hasCollisionCut ||
        state.mapPositiveHalf != mapPositiveHalf || planeMoved ||
        !MatricesNearlyEqual(state.collisionRemoteWorldTransform,
            portalWorldTransform);
    if (!needsCollisionCut)
        return;

    auto [positiveHalf, negativeHalf] = Mesh::SliceByPlane(
        state.baseVertices, localPlanePoint, localPlaneNormal);
    if (positiveHalf.empty() || negativeHalf.empty())
    {
        ResetTraversalMeshDeformation(traversingBody);
        return;
    }

    // Express the complete source-to-target mapping in the mesh's local
    // frame. This remains correct when the body or one of its parents has
    // rotation/non-uniform scale, unlike applying a world rotation to normals
    // and then attempting to undo it piecemeal.
    const glm::mat4 localToRemoteLocal = bodyWorldInverse *
        portalWorldTransform * bodyWorld;
    const glm::mat3 remoteNormalMatrix = glm::transpose(glm::inverse(
        glm::mat3(localToRemoteLocal)));

    // Negative-to-positive motion exposes the positive half through the
    // target; positive-to-negative motion exposes the negative half. Keeping
    // this selection directional prevents reverse crossings from stretching
    // the trailing (wrong) side of an asymmetric aperture.
    std::vector<Mesh::Vertex>& remoteHalf = mapPositiveHalf ? positiveHalf : negativeHalf;
    std::vector<Mesh::Vertex>& localHalf = mapPositiveHalf ? negativeHalf : positiveHalf;
    std::vector<glm::vec3> localVertices;
    localVertices.reserve(localHalf.size());
    for (const Mesh::Vertex& vertex : localHalf)
        localVertices.emplace_back(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
    std::vector<glm::vec3> remoteWorldVertices;
    remoteWorldVertices.reserve(remoteHalf.size());
    for (Mesh::Vertex& vertex : remoteHalf)
    {
        const glm::vec3 localPosition(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const glm::vec3 sourceWorldPosition = glm::vec3(bodyWorld *
            glm::vec4(localPosition, 1.f));
        const glm::vec3 mappedWorldPosition = piecewiseWarp
            ? MapWorldPointThroughPortalShape(sourceWorldPosition, *target)
            : glm::vec3(portalWorldTransform *
                glm::vec4(sourceWorldPosition, 1.f));
        const glm::vec3 mappedLocal = glm::vec3(bodyWorldInverse *
            glm::vec4(mappedWorldPosition, 1.f));
        vertex.pos[0] = mappedLocal.x;
        vertex.pos[1] = mappedLocal.y;
        vertex.pos[2] = mappedLocal.z;
        remoteWorldVertices.push_back(mappedWorldPosition);

        const glm::vec3 localNormal(vertex.normal[0], vertex.normal[1],
            vertex.normal[2]);
        glm::vec3 mappedLocalNormal;
        if (piecewiseWarp)
        {
            const glm::vec3 sourceWorldNormal = SafeNormalize(
                glm::transpose(glm::inverse(glm::mat3(bodyWorld))) * localNormal,
                glm::vec3(0.f, 0.f, 1.f));
            const glm::vec3 mappedWorldNormal =
                MapWorldNormalThroughPortalShape(sourceWorldPosition,
                    sourceWorldNormal, *target);
            mappedLocalNormal = SafeNormalize(glm::transpose(glm::mat3(bodyWorld)) *
                mappedWorldNormal, glm::vec3(0.f, 0.f, 1.f));
        }
        else
        {
            mappedLocalNormal = SafeNormalize(remoteNormalMatrix * localNormal,
                glm::vec3(0.f, 0.f, 1.f));
        }
        vertex.normal[0] = mappedLocalNormal.x;
        vertex.normal[1] = mappedLocalNormal.y;
        vertex.normal[2] = mappedLocalNormal.z;
        if (piecewiseWarp)
        {
            const glm::vec3 localTangent(vertex.tangent[0], vertex.tangent[1],
                vertex.tangent[2]);
            if (glm::dot(localTangent, localTangent) > 1e-8f)
            {
                const glm::vec3 sourceWorldTangent = glm::mat3(bodyWorld) *
                    localTangent;
                const glm::vec3 mappedWorldTangent =
                    MapWorldDirectionThroughPortalShape(sourceWorldPosition,
                        sourceWorldTangent, *target);
                const glm::vec3 mappedLocalTangent = SafeNormalize(
                    glm::mat3(bodyWorldInverse) * mappedWorldTangent,
                    localTangent);
                vertex.tangent[0] = mappedLocalTangent.x;
                vertex.tangent[1] = mappedLocalTangent.y;
                vertex.tangent[2] = mappedLocalTangent.z;
            }
        }
    }

    state.localMeshVertices = localHalf;
    state.remoteMeshVertices = remoteHalf;
    state.piecewiseWarp = piecewiseWarp;
    if (piecewiseWarp)
    {
        state.remoteRenderMesh = std::make_shared<Mesh>();
        state.remoteRenderMesh->InitializeRuntimeCloneFrom(*mesh);
        state.remoteRenderMesh->SetDeformedVertices(remoteHalf);
    }
    else
    {
        state.remoteRenderMesh.reset();
    }
    state.localCollisionVertices = localVertices;
    state.remoteLinearTransform = glm::mat3(portalWorldTransform);
    state.collisionRemoteWorldTransform = portalWorldTransform;
    state.lastCollisionPlanePoint = localPlanePoint;
    state.lastCollisionPlaneNormal = localPlaneNormal;
    state.hasCollisionCut = true;
    state.mapPositiveHalf = mapPositiveHalf;
    // Keep the dynamic owner restricted to its local clipped triangles. The
    // remote triangles are retained as a separate target-space Bvh instance;
    // neither is rebuilt until the bounded cut update above requires it.
    traversingBody->SetPortalLocalMeshCollider(&state, localVertices);
    if (Owner && Owner->GetScene())
    {
        Owner->GetScene()->GetPhysics().SetPortalMeshCollider(&state,
            *traversingBody, remoteWorldVertices);
    }
}

void SpatialManipulator::MaterializeTraversalMeshSplits(
    SpatialManipulator* /*target*/)
{
    if (!materializeSplitOnDisconnect || !Owner || !Owner->GetScene())
        return;

    Engine::Scene::Scene& scene = *Owner->GetScene();

    for (auto& pair : m_traversalStates)
    {
        RigidBody* localBody = const_cast<RigidBody*>(pair.first);
        TraversalState& state = pair.second;
        if (!localBody || !localBody->Owner || !state.meshDeformed ||
            state.localMeshVertices.empty() || state.remoteMeshVertices.empty() ||
            state.localCollisionVertices.size() < 3u)
        {
            continue;
        }

        Mesh* localMesh = ResolveMeshForObject(localBody->Owner);
        if (!localMesh)
            continue;

        // Keep the local object as the local cut. Its runtime collider is
        // re-keyed to the object so ResetTraversalMeshDeformation cannot
        // restore the old, whole-object collision hull.
        localMesh->SetDeformedVertices(state.localMeshVertices);
        localBody->SetPortalLocalMeshCollider(localBody,
            state.localCollisionVertices);

        Engine::Core::Object* remoteObject = scene.AddObject(
            localBody->Owner->name + " (Portal Fragment)");
        remoteObject->Parent = localBody->Owner->Parent;
        if (remoteObject->Parent)
            remoteObject->Parent->Children.push_back(remoteObject);
        remoteObject->transform.position = localBody->Owner->transform.position;
        remoteObject->transform.rotation = localBody->Owner->transform.rotation;
        remoteObject->transform.scale = localBody->Owner->transform.scale;

        // During traversal the remote half is kept in the source object's
        // local frame so one GPU instance can draw it through the mapping.
        // Once materialized it needs its own target-frame owner transform;
        // convert vertices back to ordinary object-local coordinates first.
        const glm::mat4 localWorld =
            localBody->Owner->transform.GetWorldMatrix();
        const glm::mat4 remoteWorld =
            state.collisionRemoteWorldTransform * localWorld;
        const glm::mat4 sourceToRemoteLocal = glm::inverse(remoteWorld) *
            localWorld;
        const glm::mat3 restoreRemoteNormal = glm::transpose(
            glm::inverse(glm::mat3(sourceToRemoteLocal)));
        std::vector<Mesh::Vertex> remoteLocalVertices =
            state.remoteMeshVertices;
        for (Mesh::Vertex& vertex : remoteLocalVertices)
        {
            const glm::vec3 mappedPosition(vertex.pos[0], vertex.pos[1],
                vertex.pos[2]);
            const glm::vec3 localPosition = glm::vec3(sourceToRemoteLocal *
                glm::vec4(mappedPosition, 1.f));
            vertex.pos[0] = localPosition.x;
            vertex.pos[1] = localPosition.y;
            vertex.pos[2] = localPosition.z;

            const glm::vec3 mappedNormal(vertex.normal[0], vertex.normal[1],
                vertex.normal[2]);
            const glm::vec3 localNormal = SafeNormalize(
                restoreRemoteNormal * mappedNormal, glm::vec3(0.f, 0.f, 1.f));
            vertex.normal[0] = localNormal.x;
            vertex.normal[1] = localNormal.y;
            vertex.normal[2] = localNormal.z;
        }

        Mesh* remoteMesh = remoteObject->AddComponent<Mesh>();
        remoteMesh->InitializeRuntimeCloneFrom(*localMesh);
        remoteMesh->SetDeformedVertices(remoteLocalVertices);
        if (const Material* localMaterial =
                localBody->Owner->GetComponent<Material>())
        {
            Material* remoteMaterial = remoteObject->AddComponent<Material>();
            remoteMaterial->Deserialize(localMaterial->Serialize());
        }
        MeshObjectCollider* remoteCollider =
            remoteObject->AddComponent<MeshObjectCollider>();
        remoteCollider->convex = true;

        RigidBody* remoteBody = remoteObject->AddComponent<RigidBody>();
        remoteBody->bodyType = localBody->bodyType;
        remoteBody->mass = std::max(0.001f, localBody->mass * 0.5f);
        localBody->mass = std::max(0.001f, localBody->mass * 0.5f);
        remoteBody->useGravity = localBody->useGravity;
        remoteBody->gravityScale = localBody->gravityScale;
        remoteBody->linearDamping = localBody->linearDamping;
        remoteBody->angularDamping = localBody->angularDamping;
        remoteBody->friction = localBody->friction;
        remoteBody->restitution = localBody->restitution;
        remoteBody->continuousCollision = localBody->continuousCollision;
        remoteBody->freezePositionX = localBody->freezePositionX;
        remoteBody->freezePositionY = localBody->freezePositionY;
        remoteBody->freezePositionZ = localBody->freezePositionZ;
        remoteBody->freezeRotationX = localBody->freezeRotationX;
        remoteBody->freezeRotationY = localBody->freezeRotationY;
        remoteBody->freezeRotationZ = localBody->freezeRotationZ;
        remoteBody->collisionLayer = localBody->collisionLayer;
        remoteBody->collisionMask = localBody->collisionMask;
        remoteBody->collisionIdentifier = localBody->collisionIdentifier;
        remoteBody->frictionBehaviors = localBody->frictionBehaviors;

        glm::mat3 remoteWorldBasis(remoteWorld);
        for (int column = 0; column < 3; ++column)
            remoteWorldBasis[column] = SafeNormalize(remoteWorldBasis[column],
                glm::vec3(column == 0 ? 1.f : 0.f,
                    column == 1 ? 1.f : 0.f,
                    column == 2 ? 1.f : 0.f));
        const glm::quat remoteWorldRotation = glm::quat_cast(remoteWorldBasis);
        remoteBody->SetWorldPose(glm::vec3(remoteWorld[3]), remoteWorldRotation);
        remoteBody->SetLinearVelocity(state.remoteLinearTransform *
            localBody->GetLinearVelocity());
        remoteBody->SetAngularVelocity(state.remoteLinearTransform *
            localBody->GetAngularVelocity());

        // Subsequent reset removes only the temporary remote proxy and this
        // traversal bookkeeping; it intentionally leaves the two fragments.
        state.meshDeformed = false;
        state.baseVertices.clear();
        state.lastMesh = nullptr;
    }
}

void SpatialManipulator::UpdateTriggerTraversal(SpatialManipulator* target,
    std::unordered_set<const RigidBody*>* claimedBodies)
{
    if (!target || !Owner || !Owner->GetScene() ||
        !HasCompatiblePortalShapeWith(*target))
    {
        ResetTraversalMeshDeformation();
        return;
    }

    RigidBody* triggerBody = ResolveTraversalTriggerBody();
    std::unordered_set<const RigidBody*> activeBodies;
    for (const auto& sceneObject : Owner->GetScene()->GetObjects())
    {
        if (!sceneObject)
            continue;

        RigidBody* body = sceneObject->GetComponent<RigidBody>();
        if (!body || !body->Owner || body->Owner == Owner || body->isTrigger)
            continue;

        activeBodies.insert(body);
        TraversalState& state = m_traversalStates[body];
        const glm::vec3 bodyWorldPosition = body->Owner->transform.GetWorldPosition();
        const float currentSignedDistance = ComputeSignedDistanceToPortalPlane(
            bodyWorldPosition);
        const bool remainsInTrigger = triggerBody && triggerBody->IsOverlapping(body);
        if (!state.hasPreviousWorldPosition)
        {
            state.previousWorldPosition = bodyWorldPosition;
            state.hasPreviousWorldPosition = true;
        }
        const float previousSignedDistance = ComputeSignedDistanceToPortalPlane(
            state.previousWorldPosition);
        // A physical teleport occurs when the body anchor crosses. Continue
        // rendering its source/target chart pair afterwards until the body
        // has cleared the target aperture, otherwise the trailing half pops
        // away exactly when the anchor crosses the plane.
        const float deformationDistance = std::max(0.001f,
            traversalBlendDistance * std::max(0.f, deformationStrength));
        const glm::vec3 planePoint = bodyWorldPosition -
            currentSignedDistance * glm::vec3(GetPortalWorldFrame()[2]);
        if (state.postTeleportVisual)
        {
            const bool remainsInVisualTransition =
                std::abs(currentSignedDistance) <= deformationDistance &&
                IsWorldPointInsidePortalAperture(planePoint, 0.001f);
            if (!remainsInVisualTransition)
                ResetTraversalMeshDeformation(body);
            else
            {
                state.previousWorldPosition = bodyWorldPosition;
                continue;
            }
        }
        // Crossing and re-arm thresholds intentionally differ. A body must
        // first establish a stable side, cross the plane, and then move well
        // clear before another traversal is possible. This suppresses contact
        // jitter and stale trigger-overlap samples after SetWorldPose.
        constexpr float kCrossingDistance = 0.001f;
        constexpr float kRearmDistance = 0.025f;
        using TraversalPhase = TraversalState::Phase;

        bool crossedPortalPlane = false;
        switch (state.phase)
        {
        case TraversalPhase::Uninitialized:
            if (currentSignedDistance <= -kRearmDistance)
                state.phase = TraversalPhase::ArmedNegative;
            else if (currentSignedDistance >= kRearmDistance)
                state.phase = TraversalPhase::ArmedPositive;
            break;
        case TraversalPhase::ArmedNegative:
            crossedPortalPlane = previousSignedDistance <= kCrossingDistance &&
                currentSignedDistance >= kCrossingDistance;
            break;
        case TraversalPhase::ArmedPositive:
            crossedPortalPlane = previousSignedDistance >= -kCrossingDistance &&
                currentSignedDistance <= -kCrossingDistance;
            break;
        case TraversalPhase::Cooldown:
            if (state.waitForOverlapExit && remainsInTrigger)
                break;
            state.waitForOverlapExit = false;
            if (currentSignedDistance <= -kRearmDistance)
                state.phase = TraversalPhase::ArmedNegative;
            else if (currentSignedDistance >= kRearmDistance)
                state.phase = TraversalPhase::ArmedPositive;
            break;
        }

        if (crossedPortalPlane && claimedBodies &&
            claimedBodies->find(body) != claimedBodies->end())
        {
            // A higher-priority aperture already consumed this swept motion.
            // Keep this portal's state current, but do not split or remap it.
            crossedPortalPlane = false;
        }

        if (crossedPortalPlane)
        {
            const float distanceDelta = currentSignedDistance - previousSignedDistance;
            const float crossingT = std::abs(distanceDelta) > 1e-8f
                ? Clamp01(-previousSignedDistance / distanceDelta) : 0.5f;
            const glm::vec3 crossingPoint = glm::mix(
                state.previousWorldPosition, bodyWorldPosition, crossingT);
            // The trigger is merely a broad-phase optimization. The swept
            // crossing must land in the actual logical aperture.
            if (!IsWorldPointInsidePortalAperture(crossingPoint, 0.001f))
                crossedPortalPlane = false;
        }

        if (crossedPortalPlane)
        {
            // A body can cross the anchor plane between two physics ticks,
            // before a prior frame had an opportunity to create its visual
            // split. Create one now so high-speed traversal retains the same
            // continuous hand-off as ordinary motion.
            if (!state.meshDeformed && deformMeshOnTraversal)
                ApplyTraversalMeshDeformation(target, body,
                    currentSignedDistance > previousSignedDistance);
            const TraversalState departingVisual = state;
            const bool retainVisualSplit = departingVisual.meshDeformed &&
                departingVisual.lastMesh;
            ResetTraversalMeshDeformation(body);
            const glm::mat4 portalTransform = GetPortalWorldTransformTo(*target);
            const bool piecewiseWarp = UsesPiecewisePortalWarpTo(*target);
            glm::mat3 portalLinear(portalTransform);
            if (piecewiseWarp)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const glm::vec3 axis(column == 0, column == 1, column == 2);
                    portalLinear[column] = MapWorldDirectionThroughPortalShape(
                        bodyWorldPosition, axis, *target);
                }
            }
            glm::mat3 portalRotation(1.f);
            for (int column = 0; column < 3; ++column)
                portalRotation[column] = SafeNormalize(portalLinear[column],
                    glm::vec3(column == 0, column == 1, column == 2));

            const glm::mat4 bodyWorld = body->Owner->transform.GetWorldMatrix();
            glm::mat3 bodyWorldRotation(1.f);
            for (int column = 0; column < 3; ++column)
                bodyWorldRotation[column] = SafeNormalize(glm::vec3(bodyWorld[column]),
                    glm::vec3(column == 0, column == 1, column == 2));

            const glm::vec3 mappedPosition = piecewiseWarp
                ? MapWorldPointThroughPortalShape(bodyWorldPosition, *target)
                : glm::vec3(portalTransform * glm::vec4(bodyWorldPosition, 1.f));
            const glm::quat mappedRotation = glm::normalize(glm::quat_cast(
                portalRotation * bodyWorldRotation));
            const glm::vec3 mappedLinearVelocity = piecewiseWarp
                ? MapWorldDirectionThroughPortalShape(bodyWorldPosition,
                    body->GetLinearVelocity(), *target)
                : portalLinear * body->GetLinearVelocity();
            const glm::vec3 mappedAngularVelocity = portalRotation *
                body->GetAngularVelocity();

            // Carry the endpoint metric into the object's persistent local
            // scale. SetWorldPose calls EnsureBody after this write, causing
            // Bullet to rebuild the collider at the same new scale before the
            // mapped pose and velocities are published.
            const float portalScaleRatio = GetPortalScaleRatioTo(*target);
            if (!piecewiseWarp)
                body->Owner->transform.scale *= portalScaleRatio;
            body->SetWorldPose(mappedPosition, mappedRotation);

            // A non-similar endpoint pair cannot be represented by an object
            // transform. Bake the piecewise result into the traversing mesh
            // after choosing its target-space pose, then use that mesh as the
            // convex runtime collider so rendering and physics retain the
            // corner-specific deformation after the handoff.
            if (piecewiseWarp)
            {
                Mesh* warpedMesh = ResolveMeshForObject(body->Owner);
                if (warpedMesh && !warpedMesh->GetVertices().empty())
                {
                    std::vector<Mesh::Vertex> warpedVertices =
                        warpedMesh->GetVertices();
                    const glm::mat4 mappedBodyWorld =
                        body->Owner->transform.GetWorldMatrix();
                    const glm::mat4 mappedBodyInverse = glm::inverse(mappedBodyWorld);
                    for (Mesh::Vertex& vertex : warpedVertices)
                    {
                        const glm::vec3 localPosition(vertex.pos[0], vertex.pos[1],
                            vertex.pos[2]);
                        const glm::vec3 oldWorldPosition(bodyWorld *
                            glm::vec4(localPosition, 1.f));
                        const glm::vec3 newWorldPosition =
                            MapWorldPointThroughPortalShape(oldWorldPosition, *target);
                        const glm::vec3 newLocalPosition(mappedBodyInverse *
                            glm::vec4(newWorldPosition, 1.f));
                        vertex.pos[0] = newLocalPosition.x;
                        vertex.pos[1] = newLocalPosition.y;
                        vertex.pos[2] = newLocalPosition.z;

                        const glm::vec3 localNormal(vertex.normal[0],
                            vertex.normal[1], vertex.normal[2]);
                        const glm::vec3 oldWorldNormal = SafeNormalize(
                            glm::transpose(glm::inverse(glm::mat3(bodyWorld))) *
                                localNormal, glm::vec3(0.f, 0.f, 1.f));
                        const glm::vec3 newWorldNormal =
                            MapWorldNormalThroughPortalShape(oldWorldPosition,
                                oldWorldNormal, *target);
                        const glm::vec3 newLocalNormal = SafeNormalize(
                            glm::transpose(glm::mat3(mappedBodyWorld)) *
                                newWorldNormal, localNormal);
                        vertex.normal[0] = newLocalNormal.x;
                        vertex.normal[1] = newLocalNormal.y;
                        vertex.normal[2] = newLocalNormal.z;
                        const glm::vec3 localTangent(vertex.tangent[0],
                            vertex.tangent[1], vertex.tangent[2]);
                        if (glm::dot(localTangent, localTangent) > 1e-8f)
                        {
                            const glm::vec3 oldWorldTangent =
                                glm::mat3(bodyWorld) * localTangent;
                            const glm::vec3 newWorldTangent =
                                MapWorldDirectionThroughPortalShape(
                                    oldWorldPosition, oldWorldTangent, *target);
                            const glm::vec3 newLocalTangent = SafeNormalize(
                                glm::mat3(mappedBodyInverse) * newWorldTangent,
                                localTangent);
                            vertex.tangent[0] = newLocalTangent.x;
                            vertex.tangent[1] = newLocalTangent.y;
                            vertex.tangent[2] = newLocalTangent.z;
                        }
                    }
                    warpedMesh->SetDeformedVertices(warpedVertices);
                    for (Engine::Core::Component* component :
                        body->Owner->Components)
                    {
                        if (auto* collider = dynamic_cast<Collider*>(component))
                            collider->collisionEnabled =
                                dynamic_cast<MeshObjectCollider*>(collider) != nullptr;
                    }
                    MeshObjectCollider* meshCollider =
                        body->Owner->GetComponent<MeshObjectCollider>();
                    if (!meshCollider)
                        meshCollider = body->Owner->AddComponent<MeshObjectCollider>();
                    meshCollider->collisionEnabled = true;
                    meshCollider->convex = true;
                }
            }
            body->SetLinearVelocity(mappedLinearVelocity);
            body->SetAngularVelocity(mappedAngularVelocity);

            // Both ends suppress this body until it has moved away from the
            // destination plane. This also prevents a reciprocal target from
            // sending it straight back during the same traversal sequence.
            state.phase = TraversalPhase::Cooldown;
            state.waitForOverlapExit = true;
            state.previousWorldPosition = mappedPosition;
            state.hasPreviousWorldPosition = true;
            TraversalState& targetState = target->m_traversalStates[body];
            if (retainVisualSplit)
            {
                // Transfer only GPU visual state. Collision split instances
                // owned by the source were removed above when the physical
                // body was placed in the target chart.
                targetState.lastMesh = departingVisual.lastMesh;
                targetState.remoteRenderWorldTransform =
                    departingVisual.remoteRenderWorldTransform;
                targetState.localRenderClipPlane =
                    departingVisual.localRenderClipPlane;
                targetState.remoteRenderClipPlane =
                    departingVisual.remoteRenderClipPlane;
                targetState.localChartPortal =
                    departingVisual.localChartPortal;
                targetState.remoteChartPortal =
                    departingVisual.remoteChartPortal;
                targetState.meshDeformed = true;
                targetState.hasCollisionCut = false;
                targetState.postTeleportVisual = true;
            }
            targetState.phase = TraversalPhase::Cooldown;
            targetState.waitForOverlapExit = false;
            targetState.previousWorldPosition = mappedPosition;
            targetState.hasPreviousWorldPosition = true;
            if (claimedBodies)
                claimedBodies->insert(body);
        }

        // A destination portal remains overlapped immediately after a
        // teleport. Its cooldown state must never slice the same mesh again;
        // otherwise the two endpoints repeatedly cut each other's generated
        // vertices and the mesh grows without bound.
        // Use the authored traversal window instead of an implicit one-unit
        // threshold. Strength scales the distance at which visual splitting
        // begins while retaining a small finite minimum.
        const bool intersectsAperture =
            std::abs(currentSignedDistance) <= deformationDistance &&
            IsWorldPointInsidePortalAperture(planePoint, 0.001f);
        const float signedMotion = currentSignedDistance - previousSignedDistance;
        const bool isArmed = state.phase == TraversalPhase::ArmedNegative ||
            state.phase == TraversalPhase::ArmedPositive;
        const bool movingTowardPortal =
            (state.phase == TraversalPhase::ArmedNegative && signedMotion > 1e-5f) ||
            (state.phase == TraversalPhase::ArmedPositive && signedMotion < -1e-5f);
        // A portal is a spatial boundary, not a movement-only trigger. A
        // stationary body that already straddles the aperture needs the same
        // two chart instances and local/remote collision pieces as a moving
        // traverser. SliceByPlane below is the exact mesh intersection test;
        // this predicate merely keeps the work scoped to the aperture.
        const bool stationaryAtPortal = std::abs(signedMotion) <= 1e-5f;
        const bool shouldMaintainSplit = movingTowardPortal || stationaryAtPortal;
        const bool claimedByHigherPriorityPortal = claimedBodies &&
            claimedBodies->find(body) != claimedBodies->end();
        if (!crossedPortalPlane && intersectsAperture && isArmed &&
            shouldMaintainSplit && !claimedByHigherPriorityPortal)
        {
            // With no directional sweep, retain the anchor's side locally
            // and map the opposite half. This makes stationary cuts stable
            // rather than choosing a side based on floating-point noise.
            const bool mapPositiveHalf = movingTowardPortal
                ? signedMotion > 0.f
                : currentSignedDistance <= 0.f;
            ApplyTraversalMeshDeformation(target, body, mapPositiveHalf);
            // Mesh deformation is an exclusive, frame-local view of a body.
            // Without this claim two coincident apertures can each cache the
            // other's already split vertices, leaving the mesh stretched when
            // either side later restores its stale snapshot.
            if (claimedBodies)
                claimedBodies->insert(body);
        }
        else if (!intersectsAperture || !shouldMaintainSplit)
            ResetTraversalMeshDeformation(body);

        if (!crossedPortalPlane)
            state.previousWorldPosition = bodyWorldPosition;
    }

    for (auto it = m_traversalStates.begin(); it != m_traversalStates.end();)
    {
        if (activeBodies.find(it->first) == activeBodies.end())
        {
            ResetTraversalMeshDeformation(const_cast<RigidBody*>(it->first));
            it = m_traversalStates.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
}
