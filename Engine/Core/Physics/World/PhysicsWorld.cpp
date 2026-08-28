#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include <algorithm>
#include <memory>
#include <vector>

namespace Engine::Physics
{
struct Physics::Impl
{
    explicit Impl(Engine::Scene::Scene& owner)
        : scene(&owner), state(std::make_unique<PhysicsWorldState>()) {}
    Engine::Scene::Scene* scene = nullptr;
    std::unique_ptr<PhysicsWorldState> state;
};

Physics::Physics(Engine::Scene::Scene& scene) : m_impl(new Impl(scene)) {}
Physics::~Physics() { delete m_impl; }
void* Physics::GetInternalState() { return m_impl ? m_impl->state.get() : nullptr; }

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
          solver.get(), collisionConfiguration.get()))
{
    gContactAddedCallback = ContactFrictionAdded;
    world->setGravity(btVector3(0.f, -9.81f, 0.f));
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
    m_impl->state = std::make_unique<Engine::Physics::PhysicsWorldState>();
}
}
