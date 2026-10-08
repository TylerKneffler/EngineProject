#include "Core/Physics/Physics.h"
#include "Core/Physics/IPhysicsAdapter.h"
#include <stdexcept>

namespace Engine::Physics
{
Physics::Physics(Engine::Scene::Scene& scene)
    : Physics(scene, CreateBulletPhysicsAdapter(scene)) {}

Physics::Physics(Engine::Scene::Scene&, std::unique_ptr<IPhysicsAdapter> adapter)
    : m_adapter(std::move(adapter))
{
    if (!m_adapter)
        throw std::invalid_argument("A physics adapter is required");
}

Physics::~Physics() = default;
void Physics::Step(double deltaTime) { m_adapter->Step(deltaTime); }
void Physics::Reset() { m_adapter->Reset(); }
void Physics::ConfigureFixedStep(double seconds, uint32_t maximumSubsteps,
    uint32_t solverIterations)
{
    m_adapter->ConfigureFixedStep(seconds, maximumSubsteps, solverIterations);
}
double Physics::GetFixedStep() const { return m_adapter->GetFixedStep(); }
uint32_t Physics::GetMaximumSubsteps() const { return m_adapter->GetMaximumSubsteps(); }
uint32_t Physics::GetSolverIterations() const { return m_adapter->GetSolverIterations(); }
uint32_t Physics::GetLastSubstepCount() const { return m_adapter->GetLastSubstepCount(); }
void Physics::SetPortalMeshCollider(const void* key,
    const Engine::Components::RigidBody& owner,
    const std::vector<glm::vec3>& vertices)
{
    m_adapter->SetPortalMeshCollider(key, owner, vertices);
}
void Physics::RemovePortalMeshCollider(const void* key)
{
    m_adapter->RemovePortalMeshCollider(key);
}
size_t Physics::GetPortalMeshColliderCount(
    const Engine::Components::RigidBody& owner) const
{
    return m_adapter->GetPortalMeshColliderCount(owner);
}
void Physics::SetPortalApertureCollider(const void* key,
    const std::vector<glm::vec3>& points, const glm::vec3& normal,
    float halfWidth, float halfDepth)
{
    m_adapter->SetPortalApertureCollider(key, points, normal, halfWidth, halfDepth);
}
void Physics::RemovePortalApertureCollider(const void* key)
{
    m_adapter->RemovePortalApertureCollider(key);
}
std::vector<Physics::RaycastHit> Physics::RaycastAll(
    const glm::vec3& origin, const glm::vec3& direction,
    float distance, uint32_t mask) const
{
    return m_adapter->RaycastAll(origin, direction, distance, mask);
}
void Physics::ApplyRigidBodyAction(Engine::Components::RigidBody& body,
    RigidBodyAction action, const glm::vec3& value,
    const glm::quat& rotation)
{
    m_adapter->ApplyRigidBodyAction(body, action, value, rotation);
}
glm::vec3 Physics::ReadRigidBodyVector(
    const Engine::Components::RigidBody& body, RigidBodyVector value) const
{
    return m_adapter->ReadRigidBodyVector(body, value);
}
bool Physics::GetRigidBodyBounds(const Engine::Components::RigidBody& body,
    glm::vec3& minimum, glm::vec3& maximum) const
{
    return m_adapter->GetRigidBodyBounds(body, minimum, maximum);
}
bool Physics::GetRigidBodyHullVertex(const Engine::Components::RigidBody& body,
    size_t index, glm::vec3& position) const
{
    return m_adapter->GetRigidBodyHullVertex(body, index, position);
}
uint64_t Physics::GetRigidBodyGeneration(
    const Engine::Components::RigidBody& body) const
{
    return m_adapter->GetRigidBodyGeneration(body);
}
void Physics::NotifyRigidBodyTransformChanged(
    Engine::Components::RigidBody& body)
{
    m_adapter->NotifyRigidBodyTransformChanged(body);
}
void Physics::SetPortalLocalMeshCollider(
    Engine::Components::RigidBody& body, const void* key,
    const std::vector<glm::vec3>& vertices)
{
    m_adapter->SetPortalLocalMeshCollider(body, key, vertices);
}
void Physics::ClearPortalLocalMeshCollider(
    Engine::Components::RigidBody& body, const void* key)
{
    m_adapter->ClearPortalLocalMeshCollider(body, key);
}
bool Physics::HasPortalLocalMeshCollider(
    const Engine::Components::RigidBody& body) const
{
    return m_adapter->HasPortalLocalMeshCollider(body);
}
void Physics::DestroyRigidBody(Engine::Components::RigidBody& body)
{
    m_adapter->DestroyRigidBody(body);
}
bool Physics::IsClothSimulating(const Engine::Components::Cloth& cloth) const
{
    return m_adapter->IsClothSimulating(cloth);
}
void Physics::ResetCloth(Engine::Components::Cloth& cloth)
{
    m_adapter->ResetCloth(cloth);
}
void Physics::DestroyCloth(Engine::Components::Cloth& cloth)
{
    m_adapter->DestroyCloth(cloth);
}
bool Physics::IsIKBoneSimulating(const Engine::Components::IKBone& bone) const
{
    return m_adapter->IsIKBoneSimulating(bone);
}
bool Physics::IsIKBoneUsingSkinnedCollider(
    const Engine::Components::IKBone& bone) const
{
    return m_adapter->IsIKBoneUsingSkinnedCollider(bone);
}
bool Physics::GetIKBoneContactSeparation(
    const Engine::Components::IKBone& bone,
    const Engine::Components::RigidBody* other, float& separation) const
{
    return m_adapter->GetIKBoneContactSeparation(bone, other, separation);
}
glm::vec3 Physics::ApplyIKBoneMeshContactTorque(
    Engine::Components::IKBone& bone, const glm::vec3& torqueImpulse,
    float limitPerMass)
{
    return m_adapter->ApplyIKBoneMeshContactTorque(
        bone, torqueImpulse, limitPerMass);
}
void Physics::ResetIKBone(Engine::Components::IKBone& bone)
{
    m_adapter->ResetIKBone(bone);
}
void Physics::DestroyIKBone(Engine::Components::IKBone& bone)
{
    m_adapter->DestroyIKBone(bone);
}
IPhysicsAdapter& Physics::GetAdapter() { return *m_adapter; }
const IPhysicsAdapter& Physics::GetAdapter() const { return *m_adapter; }
const char* Physics::GetAdapterName() const { return m_adapter->Name(); }
}
