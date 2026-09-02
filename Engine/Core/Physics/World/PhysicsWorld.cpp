#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Engine::Physics
{
namespace
{
void AddPortalRimBox(btTriangleMesh& triangles, const glm::vec3& first,
    const glm::vec3& second, const glm::vec3& normal, float halfWidth,
    float halfDepth)
{
    const glm::vec3 edge = second - first;
    const float edgeLength = glm::length(edge);
    if (edgeLength <= 1e-5f)
        return;
    const glm::vec3 tangent = edge / edgeLength;
    const glm::vec3 side = glm::normalize(glm::cross(normal, tangent)) *
        std::max(halfWidth, 0.001f);
    const glm::vec3 depth = glm::normalize(normal) *
        std::max(halfDepth, 0.001f);
    const std::array<glm::vec3, 8> vertices {
        first - side - depth, second - side - depth,
        second + side - depth, first + side - depth,
        first - side + depth, second - side + depth,
        second + side + depth, first + side + depth
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
    struct PortalMeshCollider
    {
        const Engine::Components::RigidBody* owner = nullptr;
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
    RemovePortalMeshCollider(instanceKey);
    auto* ownerObject = static_cast<btCollisionObject*>(
        owner.GetNativeCollisionObjectForPhysics());
    if (!ownerObject || worldVertices.size() < 3u)
        return;

    Impl::PortalMeshCollider collider;
    collider.owner = &owner;
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
    RemovePortalApertureCollider(instanceKey);
    if (worldPoints.size() < 3u || glm::length(worldNormal) <= 1e-5f)
        return;

    Impl::PortalMeshCollider collider;
    collider.triangles = std::make_unique<btTriangleMesh>();
    const glm::vec3 normal = glm::normalize(worldNormal);
    for (size_t index = 0; index < worldPoints.size(); ++index)
        AddPortalRimBox(*collider.triangles, worldPoints[index],
            worldPoints[(index + 1u) % worldPoints.size()], normal,
            edgeHalfWidth, edgeHalfDepth);
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

void Physics::Step(float deltaTime)
{
    Engine::Scene::Scene& scene = *m_impl->scene;
    std::vector<Engine::Components::RigidBody*> bodies;
    std::vector<Engine::Components::Cloth*> clothBodies;
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
                    body->ApplyBodySettings();
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
    if (bodies.empty() && clothBodies.empty()) return;

    Engine::Physics::PhysicsWorldState& physics = *m_impl->state;
    physics.world->stepSimulation(std::clamp(deltaTime, 0.f, 0.1f), 6, 1.f / 60.f);

    for (Engine::Components::RigidBody* body : bodies)
    {
        if (Engine::Physics::IsDynamic(*body))
            body->SyncTransformFromBody();
        body->m_editorTransformChanged = false;
    }
    for (Engine::Components::Cloth* cloth : clothBodies)
        if (!cloth->collisionMorph && cloth->IsSimulating())
            cloth->SyncMeshFromSoftBody();

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
                if (point.m_normalWorldOnB.y() > 0.5f) first->m_isGrounded = true;
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
                if (-point.m_normalWorldOnB.y() > 0.5f) second->m_isGrounded = true;
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
            cloth->UpdateCollisionMorph(deltaTime);
}

void Physics::Reset()
{
    Engine::Scene::Scene& scene = *m_impl->scene;
    for (const auto& object : scene.GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* body = dynamic_cast<Engine::Components::RigidBody*>(component)) body->DestroyBody();
            else if (auto* cloth = dynamic_cast<Engine::Components::Cloth*>(component)) cloth->DestroySoftBody(true);
    for (const auto& pair : m_impl->portalMeshColliders)
        if (pair.second.object)
            m_impl->state->world->removeCollisionObject(pair.second.object.get());
    m_impl->portalMeshColliders.clear();
    for (const auto& pair : m_impl->portalApertureColliders)
        if (pair.second.object)
            m_impl->state->world->removeCollisionObject(pair.second.object.get());
    m_impl->portalApertureColliders.clear();
    m_impl->state = std::make_unique<Engine::Physics::PhysicsWorldState>();
}
}
