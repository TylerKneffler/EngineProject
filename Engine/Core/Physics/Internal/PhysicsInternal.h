#pragma once

#include <btBulletDynamicsCommon.h>
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#include <BulletSoftBody/btSoftRigidDynamicsWorld.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

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
