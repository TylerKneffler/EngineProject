#pragma once

#include <btBulletDynamicsCommon.h>
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#include <BulletSoftBody/btSoftRigidDynamicsWorld.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Engine::Components
{
class Mesh;
class RigidBody;
}
namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Physics
{
// Owns the Bullet objects that make up one scene's isolated physics world.
class PhysicsWorldState final
{
public:
    PhysicsWorldState();

    std::unique_ptr<btSoftBodyRigidBodyCollisionConfiguration> collisionConfiguration;
    std::unique_ptr<btCollisionDispatcher> dispatcher;
    std::unique_ptr<btDbvtBroadphase> broadphase;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
    std::unique_ptr<btSoftRigidDynamicsWorld> world;
    // Prevent a remote portal proxy from colliding with the body that owns
    // the local half. All other objects still receive ordinary Bullet
    // contacts against that target-space geometry.
    class PortalProxyFilter final : public btOverlapFilterCallback
    {
    public:
        bool needBroadphaseCollision(btBroadphaseProxy* first,
            btBroadphaseProxy* second) const override
        {
            const auto* firstObject = static_cast<const btCollisionObject*>(
                first ? first->m_clientObject : nullptr);
            const auto* secondObject = static_cast<const btCollisionObject*>(
                second ? second->m_clientObject : nullptr);
            const auto firstIgnored = ignoredOwner.find(firstObject);
            const auto secondIgnored = ignoredOwner.find(secondObject);
            const auto isStatic = [](const btCollisionObject* object)
            {
                if (!object)
                    return false;
                const int flags = object->getCollisionFlags();
                return (flags & btCollisionObject::CF_STATIC_OBJECT) != 0 ||
                    (flags & btCollisionObject::CF_KINEMATIC_OBJECT) != 0;
            };
            if ((portalStaticGeometry.find(firstObject) !=
                    portalStaticGeometry.end() && isStatic(secondObject)) ||
                (portalStaticGeometry.find(secondObject) !=
                    portalStaticGeometry.end() && isStatic(firstObject)))
            {
                // Portal rims and remote halves never need static-static
                // contacts. Skipping them also avoids Bullet's warning for
                // the trigger body that owns a portal aperture.
                return false;
            }
            if (firstIgnored != ignoredOwner.end() &&
                secondIgnored != ignoredOwner.end())
            {
                // Remote halves are static collision instances. They are not
                // meaningful collision partners for each other.
                return false;
            }
            if (firstIgnored != ignoredOwner.end() &&
                firstIgnored->second == secondObject)
                return false;
            return secondIgnored == ignoredOwner.end() ||
                secondIgnored->second != firstObject;
        }

        std::unordered_map<const btCollisionObject*, const btCollisionObject*>
            ignoredOwner;
        std::unordered_set<const btCollisionObject*> portalStaticGeometry;
    };
    std::unique_ptr<PortalProxyFilter> portalProxyFilter;
    btSoftBodyWorldInfo softBodyInfo{};
};

PhysicsWorldState& StateFor(Engine::Scene::Scene* scene);

std::string Lower(std::string value);
bool IsDynamic(const Engine::Components::RigidBody& body);
bool IsKinematic(const Engine::Components::RigidBody& body);
btVector3 ToBullet(const glm::vec3& value);
glm::vec3 ToGlm(const btVector3& value);
glm::vec3 WorldScale(const Engine::Core::Object& object);
btTransform ObjectWorldTransform(const Engine::Core::Object& object);
bool SameVector(const glm::vec3& first, const glm::vec3& second);
}
