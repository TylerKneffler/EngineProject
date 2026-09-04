#include "Scripts/PortalSplitAfterDelay.h"

#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>

PortalSplitAfterDelay::PortalSplitAfterDelay()
{
    SetTypeName(COMPONENT_TYPE_NAME(PortalSplitAfterDelay));
    RegisterField("traverserObjectName", traverserObjectName);
    RegisterField("destroyAfterFrames", destroyAfterFrames);
}

namespace
{
struct PortalSplitAfterDelayRegistration
{
    PortalSplitAfterDelayRegistration()
    {
        Engine::Serialization::RegisterComponentType<PortalSplitAfterDelay>(
            "PortalSplitAfterDelay");
    }
};

PortalSplitAfterDelayRegistration g_registration;

void MakeFragmentFall(Engine::Core::Object* object)
{
    if (!object || !object->GetComponent<Engine::Components::Mesh>())
        return;

    // The original body used a primitive while it was traversing. Once the
    // portal is gone, both materialized objects need their actual cut mesh for
    // collision; otherwise the original whole-cube primitive bridges space.
    if (auto* primitive = object->GetComponent<
            Engine::Components::PrimitiveObjectCollider>())
    {
        primitive->collisionEnabled = false;
    }
    if (!object->GetComponent<Engine::Components::MeshObjectCollider>())
    {
        auto* collider = object->AddComponent<
            Engine::Components::MeshObjectCollider>();
        collider->convex = true;
    }

    auto* body = object->GetComponent<Engine::Components::RigidBody>();
    if (!body)
        body = object->AddComponent<Engine::Components::RigidBody>();
    body->bodyType = "Dynamic";
    body->mass = std::max(0.05f, body->mass);
    // The fixture disables collisions while the cube is held across the
    // aperture. Restore ordinary collision participation for the permanent
    // local/remote mesh pieces before letting them fall.
    body->collisionMask = -1;
    body->useGravity = true;
    body->gravityScale = 1.f;
    body->linearDamping = 0.02f;
    body->angularDamping = 0.05f;
    // bodyType is an authored field, so an already-created Bullet body does
    // not automatically change category when it is assigned directly.
    // Recreate it before applying the initial dynamic velocities.
    body->Disabled();
    body->Enabled();
    body->SetLinearVelocity(glm::vec3(0.f));
    body->SetAngularVelocity(glm::vec3(0.f));
}
}

void PortalSplitAfterDelay::Start()
{
    m_elapsedFrames = 0;
    m_destroyed = false;
}

void PortalSplitAfterDelay::Update()
{
    if (m_destroyed || !Owner)
        return;
    if (++m_elapsedFrames < std::max(1, destroyAfterFrames))
        return;

    auto* portal = Owner->GetComponent<Engine::Components::SpatialManipulator>();
    Engine::Scene::Scene* scene = Owner->GetScene();
    if (!portal || !scene)
        return;

    // Disconnect materializes the currently GPU/physics-split object into a
    // local mesh and a "(Portal Fragment)" mesh before clearing the portal.
    portal->Disconnect();

    Engine::Core::Object* local = Owner->FindObjectInSceneByName(
        traverserObjectName);
    Engine::Core::Object* remote = Owner->FindObjectInSceneByName(
        traverserObjectName + " (Portal Fragment)");
    MakeFragmentFall(local);
    MakeFragmentFall(remote);

    m_destroyed = true;
    // Deletion is deferred to Scene's end-of-update boundary. The component
    // remains valid until this callback returns, then its normal OnDestroy()
    // path runs while the portal link has already been disconnected.
    scene->RequestRemoveObject(Owner);
}
