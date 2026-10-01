#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Engine::Physics
{
namespace
{
bool SamePoints(const std::vector<glm::vec3>& first,
    const std::vector<glm::vec3>& second)
{
    if (first.size() != second.size())
        return false;
    for (size_t index = 0; index < first.size(); ++index)
    {
        if (first[index].x != second[index].x ||
            first[index].y != second[index].y ||
            first[index].z != second[index].z)
        {
            return false;
        }
    }
    return true;
}

void AddPortalRimBox(btTriangleMesh& triangles, const glm::vec3& first,
    const glm::vec3& second, const glm::vec3& normal,
    const glm::vec3& outward, float halfWidth,
    float halfDepth)
{
    const glm::vec3 edge = second - first;
    const float edgeLength = glm::length(edge);
    if (edgeLength <= 1e-5f)
        return;
    // The authored polygon describes the usable opening. A symmetric box
    // around its edge steals half its width from that opening, so keep almost
    // all of the rim on the exterior. Let it overlap the opening by only five
    // millimetres, however: a separation here can become a real collision
    // seam where the generated rim meets an independently triangulated room
    // floor or wall, allowing a capsule to slip out beside the portal.
    const glm::vec3 inside = -glm::normalize(outward) * 0.005f;
    const glm::vec3 outside = glm::normalize(outward) *
        (std::max(halfWidth, 0.001f) * 2.f + 0.001f);
    const glm::vec3 depth = glm::normalize(normal) *
        std::max(halfDepth, 0.001f);
    const std::array<glm::vec3, 8> vertices {
        first + inside - depth, second + inside - depth,
        second + outside - depth, first + outside - depth,
        first + inside + depth, second + inside + depth,
        second + outside + depth, first + outside + depth
    };
    constexpr std::array<std::array<int, 3>, 12> faces {{
        {{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
        {{0, 1, 5}}, {{0, 5, 4}}, {{1, 2, 6}}, {{1, 6, 5}},
        {{2, 3, 7}}, {{2, 7, 6}}, {{3, 0, 4}}, {{3, 4, 7}}
    }};
    for (const auto& face : faces)
        triangles.addTriangle(ToBullet(vertices[face[0]]),
            ToBullet(vertices[face[1]]), ToBullet(vertices[face[2]]), true);
}
}

struct Physics::Impl
{
    explicit Impl(Engine::Scene::Scene& owner)
        : scene(&owner), state(std::make_unique<PhysicsWorldState>()) {}
    Engine::Scene::Scene* scene = nullptr;
    std::unique_ptr<PhysicsWorldState> state;
    double fixedStepSeconds = 1.0 / 60.0;
    double accumulatorSeconds = 0.0;
    uint32_t maximumSubsteps = 6u;
    uint32_t solverIterations = 10u;
    uint32_t lastSubstepCount = 0u;
    struct PortalMeshCollider
    {
        const Engine::Components::RigidBody* owner = nullptr;
        std::vector<glm::vec3> vertices;
        glm::vec3 normal { 0.f };
        float edgeHalfWidth = 0.f;
        float edgeHalfDepth = 0.f;
        std::unique_ptr<btTriangleMesh> triangles;
        std::unique_ptr<btBvhTriangleMeshShape> shape;
        std::unique_ptr<btCollisionObject> object;
    };
    std::unordered_map<const void*, PortalMeshCollider> portalMeshColliders;
    std::unordered_map<const void*, PortalMeshCollider> portalApertureColliders;
};

Physics::Physics(Engine::Scene::Scene& scene) : m_impl(new Impl(scene)) {}
Physics::~Physics() { delete m_impl; }
void* Physics::GetInternalState() { return m_impl ? m_impl->state.get() : nullptr; }

void Physics::ConfigureFixedStep(double seconds, uint32_t maximumSubsteps,
    uint32_t solverIterations)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        throw std::invalid_argument(
            "A physics fixed step must be finite and positive");
    if (maximumSubsteps == 0u || solverIterations == 0u)
        throw std::invalid_argument(
            "Physics substeps and solver iterations must be positive");
    m_impl->fixedStepSeconds = seconds;
    m_impl->maximumSubsteps = maximumSubsteps;
    m_impl->solverIterations = solverIterations;
    m_impl->accumulatorSeconds = 0.0;
    m_impl->lastSubstepCount = 0u;
    m_impl->state->world->getSolverInfo().m_numIterations =
        static_cast<int>(solverIterations);
}

double Physics::GetFixedStep() const { return m_impl->fixedStepSeconds; }
uint32_t Physics::GetMaximumSubsteps() const { return m_impl->maximumSubsteps; }
uint32_t Physics::GetSolverIterations() const { return m_impl->solverIterations; }
uint32_t Physics::GetLastSubstepCount() const { return m_impl->lastSubstepCount; }

void Physics::RemovePortalMeshCollider(const void* instanceKey)
{
    if (!m_impl || !instanceKey)
        return;
    const auto found = m_impl->portalMeshColliders.find(instanceKey);
    if (found == m_impl->portalMeshColliders.end())
        return;
    if (found->second.object && m_impl->state && m_impl->state->world)
    {
        m_impl->state->portalProxyFilter->ignoredOwner.erase(
            found->second.object.get());
        m_impl->state->portalProxyFilter->portalStaticGeometry.erase(
            found->second.object.get());
        m_impl->state->world->removeCollisionObject(found->second.object.get());
    }
    m_impl->portalMeshColliders.erase(found);
}

void Physics::SetPortalMeshCollider(const void* instanceKey,
    const Engine::Components::RigidBody& owner,
    const std::vector<glm::vec3>& worldVertices)
{
    if (!m_impl || !m_impl->state || !instanceKey)
        return;
    const auto existing = m_impl->portalMeshColliders.find(instanceKey);
    if (existing != m_impl->portalMeshColliders.end() &&
        existing->second.owner == &owner &&
        SamePoints(existing->second.vertices, worldVertices))
    {
        return;
    }
    RemovePortalMeshCollider(instanceKey);
    auto* ownerObject = static_cast<btCollisionObject*>(
        owner.GetNativeCollisionObjectForPhysics());
    if (!ownerObject || worldVertices.size() < 3u)
        return;

    Impl::PortalMeshCollider collider;
    collider.owner = &owner;
    collider.vertices = worldVertices;
    collider.triangles = std::make_unique<btTriangleMesh>();
    for (size_t index = 0; index + 2u < worldVertices.size(); index += 3u)
    {
        collider.triangles->addTriangle(ToBullet(worldVertices[index]),
            ToBullet(worldVertices[index + 1u]),
            ToBullet(worldVertices[index + 2u]), true);
    }
    collider.shape = std::make_unique<btBvhTriangleMeshShape>(
        collider.triangles.get(), true);
    collider.object = std::make_unique<btCollisionObject>();
    collider.object->setCollisionShape(collider.shape.get());
    collider.object->setWorldTransform(btTransform::getIdentity());

    btCollisionObject* proxy = collider.object.get();
    m_impl->state->portalProxyFilter->ignoredOwner.emplace(proxy, ownerObject);
    m_impl->state->portalProxyFilter->portalStaticGeometry.emplace(proxy);
    m_impl->state->world->addCollisionObject(proxy,
        static_cast<short>(owner.collisionLayer),
        static_cast<short>(owner.collisionMask));
    m_impl->portalMeshColliders.emplace(instanceKey, std::move(collider));
}

size_t Physics::GetPortalMeshColliderCount(
    const Engine::Components::RigidBody& owner) const
{
    if (!m_impl)
        return 0u;
    size_t count = 0u;
    for (const auto& pair : m_impl->portalMeshColliders)
        if (pair.second.owner == &owner)
            ++count;
    return count;
}

void Physics::RemovePortalApertureCollider(const void* instanceKey)
{
    if (!m_impl || !instanceKey)
        return;
    const auto found = m_impl->portalApertureColliders.find(instanceKey);
    if (found == m_impl->portalApertureColliders.end())
        return;
    if (found->second.object && m_impl->state && m_impl->state->world)
    {
        m_impl->state->portalProxyFilter->portalStaticGeometry.erase(
            found->second.object.get());
        m_impl->state->world->removeCollisionObject(found->second.object.get());
    }
    m_impl->portalApertureColliders.erase(found);
}

void Physics::SetPortalApertureCollider(const void* instanceKey,
    const std::vector<glm::vec3>& worldPoints, const glm::vec3& worldNormal,
    float edgeHalfWidth, float edgeHalfDepth)
{
    if (!m_impl || !m_impl->state || !instanceKey)
        return;
    const glm::vec3 normalizedNormal = glm::length(worldNormal) > 1e-5f
        ? glm::normalize(worldNormal) : glm::vec3(0.f);
    const auto existing = m_impl->portalApertureColliders.find(instanceKey);
    if (existing != m_impl->portalApertureColliders.end() &&
        SamePoints(existing->second.vertices, worldPoints) &&
        existing->second.normal.x == normalizedNormal.x &&
        existing->second.normal.y == normalizedNormal.y &&
        existing->second.normal.z == normalizedNormal.z &&
        existing->second.edgeHalfWidth == edgeHalfWidth &&
        existing->second.edgeHalfDepth == edgeHalfDepth)
    {
        return;
    }
    RemovePortalApertureCollider(instanceKey);
    if (worldPoints.size() < 3u || glm::length(worldNormal) <= 1e-5f)
        return;

    Impl::PortalMeshCollider collider;
    collider.vertices = worldPoints;
    collider.normal = normalizedNormal;
    collider.edgeHalfWidth = edgeHalfWidth;
    collider.edgeHalfDepth = edgeHalfDepth;
    collider.triangles = std::make_unique<btTriangleMesh>();
    const glm::vec3 normal = normalizedNormal;
    glm::vec3 centroid(0.f);
    for (const glm::vec3& point : worldPoints)
        centroid += point;
    centroid /= static_cast<float>(worldPoints.size());
    for (size_t index = 0; index < worldPoints.size(); ++index)
    {
        const glm::vec3 first = worldPoints[index];
        const glm::vec3 second = worldPoints[(index + 1u) % worldPoints.size()];
        const glm::vec3 edge = second - first;
        glm::vec3 outward = glm::cross(normal, edge);
        if (glm::length(outward) <= 1e-5f)
            continue;
        outward = glm::normalize(outward);
        // cross(normal, edge) points inward for the usual counter-clockwise
        // aperture winding.  Detect winding rather than relying on it.
        if (glm::dot(outward, centroid - (first + second) * 0.5f) > 0.f)
            outward = -outward;
        AddPortalRimBox(*collider.triangles, first, second, normal, outward,
            edgeHalfWidth, edgeHalfDepth);
    }
    collider.shape = std::make_unique<btBvhTriangleMeshShape>(
        collider.triangles.get(), true);
    collider.object = std::make_unique<btCollisionObject>();
    collider.object->setCollisionShape(collider.shape.get());
    collider.object->setWorldTransform(btTransform::getIdentity());
    m_impl->state->portalProxyFilter->portalStaticGeometry.emplace(
        collider.object.get());
    m_impl->state->world->addCollisionObject(collider.object.get(),
        static_cast<short>(-1), static_cast<short>(-1));
    m_impl->portalApertureColliders.emplace(instanceKey, std::move(collider));
}

std::vector<Physics::RaycastHit> Physics::RaycastAll(
    const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    uint32_t collisionMask) const
{
    std::vector<RaycastHit> hits;
    if (!m_impl || !m_impl->state || !m_impl->state->world ||
        !std::isfinite(maxDistance) || maxDistance <= 0.f)
        return hits;
    const float directionLength = glm::length(direction);
    if (!std::isfinite(directionLength) || directionLength <= 1e-6f)
        return hits;

    const glm::vec3 unitDirection = direction / directionLength;
    const btVector3 from = ToBullet(origin);
    const btVector3 to = ToBullet(origin + unitDirection * maxDistance);
    btCollisionWorld::AllHitsRayResultCallback callback(from, to);
    callback.m_collisionFilterMask = static_cast<short>(collisionMask);
    m_impl->state->world->rayTest(from, to, callback);
    if (!callback.hasHit())
        return hits;

    hits.reserve(callback.m_collisionObjects.size());
    for (int index = 0; index < callback.m_collisionObjects.size(); ++index)
    {
        const btCollisionObject* collisionObject =
            callback.m_collisionObjects[index];
        if (!collisionObject)
            continue;
        RaycastHit result;
        result.point = ToGlm(callback.m_hitPointWorld[index]);
        result.normal = glm::normalize(ToGlm(callback.m_hitNormalWorld[index]));
        result.distance = callback.m_hitFractions[index] * maxDistance;
        if (auto* body = static_cast<Engine::Components::RigidBody*>(
                collisionObject->getUserPointer()))
        {
            result.rigidBody = body;
            result.object = body->Owner;
        }
        for (const auto& pair : m_impl->portalMeshColliders)
        {
            if (pair.second.object.get() != collisionObject)
                continue;
            result.kind = RaycastHitKind::PortalSplitPiece;
            result.rigidBody = const_cast<Engine::Components::RigidBody*>(
                pair.second.owner);
            result.object = result.rigidBody ? result.rigidBody->Owner : nullptr;
            result.instanceKey = pair.first;
            break;
        }
        for (const auto& pair : m_impl->portalApertureColliders)
        {
            if (pair.second.object.get() != collisionObject)
                continue;
            result.kind = RaycastHitKind::PortalApertureRim;
            result.instanceKey = pair.first;
            break;
        }
        hits.push_back(result);
    }
    std::sort(hits.begin(), hits.end(), [](const RaycastHit& first,
        const RaycastHit& second) { return first.distance < second.distance; });
    return hits;
}

PhysicsWorldState& StateFor(Engine::Scene::Scene* scene)
{
    return *static_cast<PhysicsWorldState*>(
        scene->GetPhysics().GetInternalState());
}
}

namespace Engine::Physics
{
bool ContactFrictionAdded(btManifoldPoint& point,
    const btCollisionObjectWrapper* firstObject, int, int,
    const btCollisionObjectWrapper* secondObject, int, int)
{
    if (!firstObject || !secondObject)
        return false;
    auto* first = static_cast<Engine::Components::RigidBody*>(
        firstObject->getCollisionObject()->getUserPointer());
    auto* second = static_cast<Engine::Components::RigidBody*>(
        secondObject->getCollisionObject()->getUserPointer());
    if (!first || !second)
        return false;
    point.m_combinedFriction = std::clamp(
        first->ResolveFrictionFor(second) * second->ResolveFrictionFor(first),
        0.f, 1.f);
    return true;
}

PhysicsWorldState::PhysicsWorldState()
    : collisionConfiguration(std::make_unique<btSoftBodyRigidBodyCollisionConfiguration>()),
      dispatcher(std::make_unique<btCollisionDispatcher>(collisionConfiguration.get())),
      broadphase(std::make_unique<btDbvtBroadphase>()),
      solver(std::make_unique<btSequentialImpulseConstraintSolver>()),
      world(std::make_unique<btSoftRigidDynamicsWorld>(dispatcher.get(), broadphase.get(),
          solver.get(), collisionConfiguration.get())),
      portalProxyFilter(std::make_unique<PortalProxyFilter>())
{
    gContactAddedCallback = ContactFrictionAdded;
    world->setGravity(btVector3(0.f, -9.81f, 0.f));
    world->getPairCache()->setOverlapFilterCallback(portalProxyFilter.get());
    softBodyInfo.m_broadphase = broadphase.get();
    softBodyInfo.m_dispatcher = dispatcher.get();
    softBodyInfo.m_gravity = world->getGravity();
    softBodyInfo.m_sparsesdf.Initialize();
}

void Physics::Step(double deltaTime)
{
    Engine::Scene::Scene& scene = *m_impl->scene;
    std::vector<Engine::Components::RigidBody*> bodies;
    std::vector<Engine::Components::Cloth*> clothBodies;
    std::vector<Engine::Components::IKBone*> ikBones;
    for (const auto& object : scene.GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* body = dynamic_cast<Engine::Components::RigidBody*>(component))
            {
                bodies.push_back(body);
                body->m_isColliding = false;
                body->m_isGrounded = false;
                body->BeginOverlapFrame();
                if (body->EnsureBody())
                {
                    if (!Engine::Physics::IsDynamic(*body) ||
                        body->m_editorTransformChanged)
                        body->SyncBodyFromTransform();
                }
            }
            else if (auto* cloth = dynamic_cast<Engine::Components::Cloth*>(component))
            {
                clothBodies.push_back(cloth);
                if (cloth->collisionMorph)
                    cloth->EnsureCollisionMorph();
                else if (cloth->EnsureSoftBody())
                {
                    cloth->UpdatePinnedNodes();
                    cloth->ApplyForces();
                }
            }
            else if (auto* bone = dynamic_cast<
                    Engine::Components::IKBone*>(component))
                ikBones.push_back(bone);
    for (Engine::Components::IKBone* bone : ikBones)
        bone->AdvanceActivation(static_cast<float>(deltaTime));
    for (Engine::Components::IKBone* bone : ikBones)
        bone->RemoveInvalidConstraint();
    for (Engine::Components::IKBone* bone : ikBones)
        if (!bone->WantsSimulation())
            bone->DestroyBody(true);
    for (Engine::Components::IKBone* bone : ikBones)
        bone->EnsureBody();
    for (Engine::Components::IKBone* bone : ikBones)
        bone->EnsureConstraint();
    m_impl->lastSubstepCount = 0u;
    if (bodies.empty() && clothBodies.empty() && ikBones.empty())
    {
        m_impl->accumulatorSeconds = 0.0;
        return;
    }

    Engine::Physics::PhysicsWorldState& physics = *m_impl->state;
    physics.world->getSolverInfo().m_numIterations =
        static_cast<int>(m_impl->solverIterations);
    const double boundedDelta = std::clamp(
        std::isfinite(deltaTime) ? deltaTime : 0.0, 0.0,
        m_impl->fixedStepSeconds * m_impl->maximumSubsteps);
    m_impl->accumulatorSeconds = std::min(
        m_impl->accumulatorSeconds + boundedDelta,
        m_impl->fixedStepSeconds * m_impl->maximumSubsteps);
    const double epsilon = m_impl->fixedStepSeconds * 1e-9;
    while (m_impl->lastSubstepCount < m_impl->maximumSubsteps &&
        m_impl->accumulatorSeconds + epsilon >= m_impl->fixedStepSeconds)
    {
        // Reapply live settings at simulation cadence, not output cadence.
        // Besides making runtime edits visible, this prevents calls such as
        // setAngularFactor/updateInertiaTensor from making contact outcomes
        // depend on how fixed physics steps were grouped into video frames.
        for (Engine::Components::RigidBody* body : bodies)
            body->ApplyBodySettings();
        const btScalar step = static_cast<btScalar>(m_impl->fixedStepSeconds);
        // maxSubSteps = 0 disables Bullet's float accumulator. The engine's
        // double accumulator above makes chunking independent of output FPS.
        physics.world->stepSimulation(step, 0, step);
        m_impl->accumulatorSeconds -= m_impl->fixedStepSeconds;
        if (m_impl->accumulatorSeconds < 0.0 &&
            m_impl->accumulatorSeconds > -epsilon)
            m_impl->accumulatorSeconds = 0.0;
        ++m_impl->lastSubstepCount;
    }

    for (Engine::Components::RigidBody* body : bodies)
    {
        if (Engine::Physics::IsDynamic(*body))
            body->SyncTransformFromBody();
        body->m_editorTransformChanged = false;
    }
    for (Engine::Components::Cloth* cloth : clothBodies)
        if (!cloth->collisionMorph && cloth->IsSimulating())
            cloth->SyncMeshFromSoftBody();
    for (Engine::Components::IKBone* bone : ikBones)
        bone->SyncBoneFromBody();

    const int manifoldCount = physics.dispatcher->getNumManifolds();
    for (int index = 0; index < manifoldCount; ++index)
    {
        btPersistentManifold* manifold = physics.dispatcher->getManifoldByIndexInternal(index);
        auto* first = static_cast<Engine::Components::RigidBody*>(manifold->getBody0()->getUserPointer());
        auto* second = static_cast<Engine::Components::RigidBody*>(manifold->getBody1()->getUserPointer());

        bool hasPenetratingContact = false;
        for (int contact = 0; contact < manifold->getNumContacts(); ++contact)
        {
            const btManifoldPoint& point = manifold->getContactPoint(contact);
            if (point.getDistance() <= 0.f)
            {
                hasPenetratingContact = true;
                break;
            }
        }
        if (hasPenetratingContact)
        {
            if (first && second)
            {
                first->RegisterOverlap(second);
                second->RegisterOverlap(first);
            }
        }

        for (int contact = 0; contact < manifold->getNumContacts(); ++contact)
        {
            const btManifoldPoint& point = manifold->getContactPoint(contact);
            if (point.getDistance() > 0.f) continue;
            if (first)
            {
                first->m_isColliding = true;
                const glm::vec3 firstUp = -first->GetGravityDirection();
                if (glm::dot(Engine::Physics::ToGlm(point.m_normalWorldOnB),
                        firstUp) > 0.5f)
                    first->m_isGrounded = true;
                if (first->Owner)
                    if (auto* cloth = first->Owner->GetComponent<Engine::Components::Cloth>())
                        cloth->NotifyRigidBodyCollision(
                            Engine::Physics::ToGlm(point.m_normalWorldOnB),
                            Engine::Physics::ToGlm(point.getPositionWorldOnA()),
                            point.getAppliedImpulse());
            }
            if (second)
            {
                second->m_isColliding = true;
                const glm::vec3 secondUp = -second->GetGravityDirection();
                if (glm::dot(-Engine::Physics::ToGlm(point.m_normalWorldOnB),
                        secondUp) > 0.5f)
                    second->m_isGrounded = true;
                if (second->Owner)
                    if (auto* cloth = second->Owner->GetComponent<Engine::Components::Cloth>())
                        cloth->NotifyRigidBodyCollision(
                            -Engine::Physics::ToGlm(point.m_normalWorldOnB),
                            Engine::Physics::ToGlm(point.getPositionWorldOnB()),
                            point.getAppliedImpulse());
            }
        }
    }
    for (Engine::Components::Cloth* cloth : clothBodies)
        if (cloth->collisionMorph)
            cloth->UpdateCollisionMorph(static_cast<float>(boundedDelta));
}

void Physics::Reset()
{
    Engine::Scene::Scene& scene = *m_impl->scene;
    for (const auto& object : scene.GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* body = dynamic_cast<Engine::Components::RigidBody*>(component)) body->DestroyBody();
            else if (auto* cloth = dynamic_cast<Engine::Components::Cloth*>(component)) cloth->DestroySoftBody(true);
            else if (auto* bone = dynamic_cast<Engine::Components::IKBone*>(component)) bone->DestroyConstraint(true);
    for (const auto& object : scene.GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* bone = dynamic_cast<Engine::Components::IKBone*>(component)) bone->DestroyBody(true);
    for (const auto& pair : m_impl->portalMeshColliders)
        if (pair.second.object)
            m_impl->state->world->removeCollisionObject(pair.second.object.get());
    m_impl->portalMeshColliders.clear();
    for (const auto& pair : m_impl->portalApertureColliders)
        if (pair.second.object)
            m_impl->state->world->removeCollisionObject(pair.second.object.get());
    m_impl->portalApertureColliders.clear();
    m_impl->state = std::make_unique<Engine::Physics::PhysicsWorldState>();
    m_impl->state->world->getSolverInfo().m_numIterations =
        static_cast<int>(m_impl->solverIterations);
    m_impl->accumulatorSeconds = 0.0;
    m_impl->lastSubstepCount = 0u;
}
}
