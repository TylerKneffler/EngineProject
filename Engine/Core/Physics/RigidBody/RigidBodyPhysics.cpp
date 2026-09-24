#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include "Editor/UI/IEditorUi.h"
#include <btBulletDynamicsCommon.h>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Components
{
struct Engine::Components::RigidBody::Impl
{
    Engine::Scene::Scene* scene = nullptr;
    uint64_t configurationRevision = 0;
    glm::vec3 worldScale{};
    std::vector<std::pair<const Engine::Components::Collider*, uint64_t>> colliders;
    const Engine::Components::Mesh* ownerMesh = nullptr;
    uint64_t ownerMeshRevision = 0;
    const void* portalLocalMeshKey = nullptr;
    std::vector<glm::vec3> portalLocalMeshVertices;
    uint64_t portalLocalMeshRevision = 0;
    uint64_t activePortalLocalMeshRevision = 0;
    uint64_t syncedWorldRevision = 0;
    std::unique_ptr<btCompoundShape> compound;
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btTriangleMesh>> triangleMeshes;
    std::unique_ptr<btDefaultMotionState> motionState;
    std::unique_ptr<btRigidBody> body;
    std::unordered_set<const Engine::Components::RigidBody*> currentOverlaps;
    std::unordered_set<const Engine::Components::RigidBody*> previousOverlaps;
};

Engine::Components::RigidBody::RigidBody() : m_impl(new Impl())
{
    SetTypeName("RigidBody");
    singlecomponent = true;
    RegisterField("bodyType", bodyType, "Body");
    RegisterField("mass", mass, "Body");
    RegisterField("useGravity", useGravity, "Forces");
    RegisterField("gravityScale", gravityScale, "Forces");
    RegisterField("gravityDirection", gravityDirection, "Forces");
    RegisterField("linearDamping", linearDamping, "Forces");
    RegisterField("angularDamping", angularDamping, "Forces");
    RegisterField("friction", friction, "Material");
    RegisterField("restitution", restitution, "Material");
    RegisterField("isTrigger", isTrigger, "Collision");
    RegisterField("continuousCollision", continuousCollision, "Collision");
    RegisterField("initialLinearVelocity", initialLinearVelocity, "Initial Velocity", false);
    RegisterField("initialAngularVelocity", initialAngularVelocity, "Initial Velocity", false);
    RegisterField("freezePositionX", freezePositionX, "Constraints", false);
    RegisterField("freezePositionY", freezePositionY, "Constraints", false);
    RegisterField("freezePositionZ", freezePositionZ, "Constraints", false);
    RegisterField("freezeRotationX", freezeRotationX, "Constraints", false);
    RegisterField("freezeRotationY", freezeRotationY, "Constraints", false);
    RegisterField("freezeRotationZ", freezeRotationZ, "Constraints", false);
    RegisterField("collisionLayer", collisionLayer, "Filtering", false);
    RegisterField("collisionMask", collisionMask, "Filtering", false);
    RegisterField("collisionIdentifier", collisionIdentifier, "Filtering", false);
}

bool Engine::Components::RigidBody::IsOverlapping(const RigidBody* other) const
{
    if (!m_impl || !other)
        return false;
    return m_impl->currentOverlaps.find(other) != m_impl->currentOverlaps.end();
}

bool Engine::Components::RigidBody::DidBeginOverlap(const RigidBody* other) const
{
    if (!m_impl || !other)
        return false;
    return m_impl->currentOverlaps.find(other) != m_impl->currentOverlaps.end() &&
        m_impl->previousOverlaps.find(other) == m_impl->previousOverlaps.end();
}

bool Engine::Components::RigidBody::DidEndOverlap(const RigidBody* other) const
{
    if (!m_impl || !other)
        return false;
    return m_impl->currentOverlaps.find(other) == m_impl->currentOverlaps.end() &&
        m_impl->previousOverlaps.find(other) != m_impl->previousOverlaps.end();
}

std::vector<Engine::Components::RigidBody*> Engine::Components::RigidBody::GetOverlappingBodies() const
{
    std::vector<Engine::Components::RigidBody*> overlaps;
    if (!m_impl)
        return overlaps;
    overlaps.reserve(m_impl->currentOverlaps.size());
    for (const Engine::Components::RigidBody* body : m_impl->currentOverlaps)
        overlaps.push_back(const_cast<Engine::Components::RigidBody*>(body));
    return overlaps;
}

void Engine::Components::RigidBody::BeginOverlapFrame()
{
    if (!m_impl)
        return;
    m_impl->previousOverlaps = m_impl->currentOverlaps;
    m_impl->currentOverlaps.clear();
}

void Engine::Components::RigidBody::RegisterOverlap(const RigidBody* other)
{
    if (!m_impl || !other || other == this)
        return;
    m_impl->currentOverlaps.insert(other);
}

Engine::Components::RigidBody::~RigidBody()
{
    DestroyBody();
    delete m_impl;
}

bool Engine::Components::RigidBody::EnsureBody()
{
    if (!Owner || !Owner->GetScene() || !Owner->IsEnabledInHierarchy())
    {
        DestroyBody();
        return false;
    }
    const glm::vec3 scale = Engine::Physics::WorldScale(*Owner);
    bool usesOwnerMeshCollider = false;
    for (Engine::Core::Component* component : Owner->Components)
    {
        const auto* meshCollider = dynamic_cast<
            const Engine::Components::MeshObjectCollider*>(component);
        if (meshCollider && !meshCollider->meshReference.IsAssigned() &&
            meshCollider->meshPath.empty())
        {
            usesOwnerMeshCollider = true;
            break;
        }
    }
    const Engine::Components::Mesh* ownerMesh = usesOwnerMeshCollider
        ? Owner->GetComponent<Engine::Components::Mesh>() : nullptr;
    bool configurationMatches = m_impl->body &&
        m_impl->configurationRevision == GetConfigurationRevision() &&
        m_impl->activePortalLocalMeshRevision ==
            m_impl->portalLocalMeshRevision &&
        Engine::Physics::SameVector(m_impl->worldScale, scale) &&
        m_impl->ownerMesh == ownerMesh &&
        m_impl->ownerMeshRevision ==
            (ownerMesh ? ownerMesh->GetConfigurationRevision() : 0);
    size_t colliderIndex = 0;
    // The active portal piece replaces the shapes used by the body, but the
    // authored colliders still participate in configuration invalidation.
    // Never mutate the current Bullet compound while merely checking whether
    // it can be reused.
    for (Engine::Core::Component* component : Owner->Components)
    {
        const auto* collider =
            dynamic_cast<const Engine::Components::Collider*>(component);
        if (!collider)
            continue;
        if (colliderIndex >= m_impl->colliders.size() ||
            m_impl->colliders[colliderIndex].first != collider ||
            m_impl->colliders[colliderIndex].second !=
                collider->GetConfigurationRevision())
        {
            configurationMatches = false;
        }
        ++colliderIndex;
    }
    configurationMatches = configurationMatches &&
        colliderIndex == m_impl->colliders.size();
    if (configurationMatches)
        return true;

    // Runtime mesh cuts and collider edits rebuild the Bullet body. Preserve
    // its live kinematics rather than restoring authoring-time initial values;
    // otherwise a portal traversal can correctly remap velocity and then have
    // that remap overwritten on the following physics step.
    const bool preserveKinematics = m_impl->body != nullptr;
    const glm::vec3 preservedLinearVelocity = preserveKinematics
        ? Engine::Physics::ToGlm(m_impl->body->getLinearVelocity())
        : glm::vec3(0.f);
    const glm::vec3 preservedAngularVelocity = preserveKinematics
        ? Engine::Physics::ToGlm(m_impl->body->getAngularVelocity())
        : glm::vec3(0.f);
    DestroyBody();

    m_impl->scene = Owner->GetScene();
    m_impl->compound = std::make_unique<btCompoundShape>();

    // A body that intersects a portal is represented by two independent
    // collision pieces.  The remote piece is registered by PhysicsWorld in
    // the connected chart; this local body must contain *only* the local cut
    // or its original primitive/mesh collider will bridge the aperture and
    // continue colliding on the remote side.  The old code tracked the cut
    // revision but then rebuilt every authored collider, silently discarding
    // the split geometry.
    const bool hasPortalLocalPiece = m_impl->portalLocalMeshKey &&
        m_impl->portalLocalMeshVertices.size() >= 3u;
    if (hasPortalLocalPiece)
    {
        auto hull = std::make_unique<btConvexHullShape>();
        for (const glm::vec3& vertex : m_impl->portalLocalMeshVertices)
        {
            hull->addPoint(btVector3(vertex.x * scale.x, vertex.y * scale.y,
                vertex.z * scale.z), false);
        }
        hull->recalcLocalAabb();
        m_impl->compound->addChildShape(btTransform::getIdentity(), hull.get());
        m_impl->shapes.push_back(std::move(hull));
    }
    else for (Engine::Core::Component* component : Owner->Components)
    {
        Engine::Components::Collider* collider = dynamic_cast<Engine::Components::Collider*>(component);
        if (!collider || !collider->collisionEnabled) continue;
        btTransform child;
        child.setIdentity();
        child.setOrigin(Engine::Physics::ToBullet(collider->center * scale));

        std::unique_ptr<btCollisionShape> shape;
        if (auto* primitive = dynamic_cast<PrimitiveObjectCollider*>(collider))
        {
            const std::string kind = Engine::Physics::Lower(primitive->shape);
            const glm::vec3 size = glm::max(glm::abs(primitive->size * scale), glm::vec3(0.001f));
            if (kind == "sphere" || kind == "circle")
            {
                const float radius = std::max(0.001f, primitive->radius *
                    std::max({ std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) }));
                shape = std::make_unique<btSphereShape>(radius);
            }
            else if (kind == "capsule")
            {
                const float radius = std::max(0.001f, primitive->radius *
                    std::max(std::abs(scale.x), std::abs(scale.z)));
                const float totalHeight = std::max(radius * 2.f,
                    primitive->height * std::abs(scale.y));
                shape = std::make_unique<btCapsuleShape>(radius, totalHeight - radius * 2.f);
            }
            else if (kind == "cylinder")
            {
                const float radius = std::max(0.001f, primitive->radius *
                    std::max(std::abs(scale.x), std::abs(scale.z)));
                shape = std::make_unique<btCylinderShape>(btVector3(radius,
                    std::max(0.001f, primitive->height * std::abs(scale.y) * 0.5f), radius));
            }
            else
                shape = std::make_unique<btBoxShape>(Engine::Physics::ToBullet(size * 0.5f));
        }
        else if (auto* meshCollider = dynamic_cast<MeshObjectCollider*>(collider))
        {
            std::unique_ptr<Engine::Components::Mesh> loadedMesh;
            const Engine::Components::Mesh* mesh = nullptr;
            if (meshCollider->meshReference.IsAssigned())
                mesh = Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(Owner, meshCollider->meshReference);
            else if (!meshCollider->meshPath.empty())
            {
                loadedMesh = std::make_unique<Engine::Components::Mesh>();
                try { loadedMesh->LoadFromFile(meshCollider->meshPath); }
                catch (...) { loadedMesh.reset(); }
                mesh = loadedMesh.get();
            }
            else
                mesh = Owner->GetComponent<Engine::Components::Mesh>();
            const bool indexedTerrain = mesh &&
                mesh->UsesTerrainVertexFormat() &&
                !mesh->GetTerrainVertices().empty() &&
                !mesh->GetIndices().empty();
            if (!mesh || (mesh->GetVertices().empty() && !indexedTerrain))
                continue;

            const bool convex = meshCollider->convex || Engine::Physics::IsDynamic(*this);
            if (convex)
            {
                auto hull = std::make_unique<btConvexHullShape>();
                if (indexedTerrain)
                {
                    for (const Engine::Model::TerrainVertex& vertex :
                        mesh->GetTerrainVertices())
                    {
                        hull->addPoint(btVector3(vertex.pos[0] * scale.x,
                            vertex.pos[1] * scale.y,
                            vertex.pos[2] * scale.z), false);
                    }
                }
                else
                {
                    for (const Engine::Model::AnimationVertex& vertex :
                        mesh->GetVertices())
                    {
                        hull->addPoint(btVector3(vertex.pos[0] * scale.x,
                            vertex.pos[1] * scale.y,
                            vertex.pos[2] * scale.z), false);
                    }
                }
                hull->recalcLocalAabb();
                shape = std::move(hull);
            }
            else
            {
                auto triangles = std::make_unique<btTriangleMesh>();
                if (indexedTerrain)
                {
                    const auto& vertices = mesh->GetTerrainVertices();
                    const auto& indices = mesh->GetIndices();
                    const auto point = [&vertices, &scale](uint32_t index)
                    {
                        return btVector3(vertices[index].pos[0] * scale.x,
                            vertices[index].pos[1] * scale.y,
                            vertices[index].pos[2] * scale.z);
                    };
                    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                    {
                        if (indices[i] >= vertices.size() ||
                            indices[i + 1] >= vertices.size() ||
                            indices[i + 2] >= vertices.size())
                            continue;
                        triangles->addTriangle(point(indices[i]),
                            point(indices[i + 1]), point(indices[i + 2]));
                    }
                }
                else
                {
                    const auto& vertices = mesh->GetVertices();
                    const auto point = [&vertices, &scale](std::size_t index)
                    {
                        return btVector3(vertices[index].pos[0] * scale.x,
                            vertices[index].pos[1] * scale.y,
                            vertices[index].pos[2] * scale.z);
                    };
                    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3)
                        triangles->addTriangle(point(i), point(i + 1),
                            point(i + 2));
                }
                shape = std::make_unique<btBvhTriangleMeshShape>(triangles.get(), true);
                m_impl->triangleMeshes.push_back(std::move(triangles));
            }
        }

        if (shape)
        {
            m_impl->compound->addChildShape(child, shape.get());
            m_impl->shapes.push_back(std::move(shape));
        }
    }

    if (m_impl->compound->getNumChildShapes() == 0)
    {
        DestroyBody();
        return false;
    }

    const bool dynamic = Engine::Physics::IsDynamic(*this);
    const btScalar bodyMass = dynamic ? std::max(0.001f, mass) : 0.f;
    btVector3 inertia(0.f, 0.f, 0.f);
    if (bodyMass > 0.f) m_impl->compound->calculateLocalInertia(bodyMass, inertia);
    const btTransform transform = Engine::Physics::ObjectWorldTransform(*Owner);
    m_impl->motionState = std::make_unique<btDefaultMotionState>(transform);
    btRigidBody::btRigidBodyConstructionInfo info(bodyMass, m_impl->motionState.get(),
        m_impl->compound.get(), inertia);
    m_impl->body = std::make_unique<btRigidBody>(info);
    m_impl->body->setUserPointer(this);
    m_impl->body->setCollisionFlags(m_impl->body->getCollisionFlags() |
        btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
    if (Engine::Physics::IsKinematic(*this))
    {
        m_impl->body->setCollisionFlags(m_impl->body->getCollisionFlags() |
            btCollisionObject::CF_KINEMATIC_OBJECT);
        m_impl->body->setActivationState(DISABLE_DEACTIVATION);
    }
    if (isTrigger)
        m_impl->body->setCollisionFlags(m_impl->body->getCollisionFlags() |
            btCollisionObject::CF_NO_CONTACT_RESPONSE);
    ApplyBodySettings();
    m_impl->body->setLinearVelocity(Engine::Physics::ToBullet(
        preserveKinematics ? preservedLinearVelocity : initialLinearVelocity));
    m_impl->body->setAngularVelocity(Engine::Physics::ToBullet(
        preserveKinematics ? preservedAngularVelocity : initialAngularVelocity));
    Engine::Physics::StateFor(m_impl->scene).world->addRigidBody(m_impl->body.get(),
        static_cast<short>(collisionLayer), static_cast<short>(collisionMask));
    m_impl->configurationRevision = GetConfigurationRevision();
    m_impl->syncedWorldRevision = Owner->transform.GetWorldRevision();
    m_impl->worldScale = scale;
    m_impl->ownerMesh = ownerMesh;
    m_impl->ownerMeshRevision = ownerMesh
        ? ownerMesh->GetConfigurationRevision() : 0;
    m_impl->activePortalLocalMeshRevision = m_impl->portalLocalMeshRevision;
    m_impl->colliders.clear();
    for (Engine::Core::Component* component : Owner->Components)
        if (const auto* collider =
            dynamic_cast<const Engine::Components::Collider*>(component))
            m_impl->colliders.emplace_back(
                collider, collider->GetConfigurationRevision());
    return true;
}

void Engine::Components::RigidBody::DestroyBody()
{
    if (!m_impl) return;
    if (m_impl->body && m_impl->scene)
    {
        Engine::Physics::StateFor(m_impl->scene).world->removeRigidBody(m_impl->body.get());
    }
    m_impl->body.reset();
    m_impl->motionState.reset();
    m_impl->compound.reset();
    m_impl->shapes.clear();
    m_impl->triangleMeshes.clear();
    m_impl->scene = nullptr;
    m_impl->configurationRevision = 0;
    m_impl->syncedWorldRevision = 0;
    m_impl->worldScale = {};
    m_impl->colliders.clear();
    m_impl->ownerMesh = nullptr;
    m_impl->ownerMeshRevision = 0;
    m_impl->activePortalLocalMeshRevision = 0;
    m_impl->currentOverlaps.clear();
    m_impl->previousOverlaps.clear();
    m_isColliding = false;
    m_isGrounded = false;
}

void Engine::Components::RigidBody::ApplyBodySettings()
{
    if (!m_impl || !m_impl->body) return;
    m_impl->body->setDamping(std::max(0.f, linearDamping), std::max(0.f, angularDamping));
    m_impl->body->setFriction(std::clamp(friction, 0.f, 1.f));
    m_impl->body->setRestitution(std::clamp(restitution, 0.f, 1.f));
    const glm::vec3 gravity = GetGravityDirection() * (9.81f * gravityScale);
    m_impl->body->setGravity(useGravity ? Engine::Physics::ToBullet(gravity)
                                        : btVector3(0.f, 0.f, 0.f));
    m_impl->body->setLinearFactor(btVector3(!freezePositionX, !freezePositionY, !freezePositionZ));
    m_impl->body->setAngularFactor(btVector3(!freezeRotationX, !freezeRotationY, !freezeRotationZ));
    if (continuousCollision)
    {
        m_impl->body->setCcdMotionThreshold(0.001f);
        m_impl->body->setCcdSweptSphereRadius(0.1f);
    }
    else
        m_impl->body->setCcdMotionThreshold(0.f);
}

void* Engine::Components::RigidBody::GetNativeCollisionObjectForPhysics() const
{
    return m_impl && m_impl->body ? m_impl->body.get() : nullptr;
}

void Engine::Components::RigidBody::SyncBodyFromTransform()
{
    if (!m_impl || !m_impl->body || !Owner) return;
    const uint64_t worldRevision = Owner->transform.GetWorldRevision();
    if (m_impl->syncedWorldRevision == worldRevision)
        return;
    const btTransform transform = Engine::Physics::ObjectWorldTransform(*Owner);
    m_impl->body->setWorldTransform(transform);
    if (m_impl->motionState) m_impl->motionState->setWorldTransform(transform);
    m_impl->body->activate(true);
    m_impl->syncedWorldRevision = worldRevision;
}

void Engine::Components::RigidBody::NotifyEditorTransformChanged()
{
    m_editorTransformChanged = true;
    if (EnsureBody())
        SyncBodyFromTransform();
}

void Engine::Components::RigidBody::SetPortalLocalMeshCollider(
    const void* instanceKey, const std::vector<glm::vec3>& localVertices)
{
    if (!m_impl)
        return;
    if (!instanceKey || localVertices.size() < 3u)
    {
        ClearPortalLocalMeshCollider(instanceKey);
        return;
    }
    const bool unchanged = m_impl->portalLocalMeshKey == instanceKey &&
        m_impl->portalLocalMeshVertices.size() == localVertices.size() &&
        std::equal(m_impl->portalLocalMeshVertices.begin(),
            m_impl->portalLocalMeshVertices.end(), localVertices.begin(),
            [](const glm::vec3& first, const glm::vec3& second)
            {
                return first.x == second.x && first.y == second.y &&
                    first.z == second.z;
            });
    if (unchanged)
        return;
    m_impl->portalLocalMeshKey = instanceKey;
    m_impl->portalLocalMeshVertices = localVertices;
    if (++m_impl->portalLocalMeshRevision == 0)
        ++m_impl->portalLocalMeshRevision;
    EnsureBody();
}

void Engine::Components::RigidBody::ClearPortalLocalMeshCollider(
    const void* instanceKey)
{
    if (!m_impl || !m_impl->portalLocalMeshKey ||
        (instanceKey && m_impl->portalLocalMeshKey != instanceKey))
    {
        return;
    }
    m_impl->portalLocalMeshKey = nullptr;
    m_impl->portalLocalMeshVertices.clear();
    if (++m_impl->portalLocalMeshRevision == 0)
        ++m_impl->portalLocalMeshRevision;
    EnsureBody();
}

bool Engine::Components::RigidBody::HasPortalLocalMeshCollider() const
{
    return m_impl && m_impl->portalLocalMeshKey &&
        m_impl->portalLocalMeshVertices.size() >= 3u;
}

void Engine::Components::RigidBody::SyncTransformFromBody()
{
    if (!m_impl || !m_impl->body || !Owner) return;
    const btTransform transform = m_impl->body->getWorldTransform();
    const btQuaternion bulletRotation = transform.getRotation();
    const glm::quat worldRotation(bulletRotation.w(), bulletRotation.x(),
        bulletRotation.y(), bulletRotation.z());
    glm::mat4 world = glm::mat4_cast(worldRotation);
    const btVector3 origin = transform.getOrigin();
    world[3] = glm::vec4(origin.x(), origin.y(), origin.z(), 1.f);
    const glm::mat4 parent = Owner->Parent
        ? Owner->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parent) * world;
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (glm::decompose(local, scale, rotation, translation, skew, perspective))
    {
        Owner->transform.position = translation;
        Owner->transform.rotation = glm::eulerAngles(glm::normalize(rotation));
        // Observe the direct field writes so all later systems see the new
        // revision, while recording that Bullet already owns this transform.
        m_impl->syncedWorldRevision = Owner->transform.GetWorldRevision();
    }
}

void Engine::Components::RigidBody::Start() {}
void Engine::Components::RigidBody::Update() {}
void Engine::Components::RigidBody::Enabled() {}
void Engine::Components::RigidBody::Disabled() { DestroyBody(); }
void Engine::Components::RigidBody::OnDestroy() { DestroyBody(); }

void Engine::Components::RigidBody::AddForce(const glm::vec3& force)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyCentralForce(Engine::Physics::ToBullet(force)); }
}
void Engine::Components::RigidBody::AddTorque(const glm::vec3& torque)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyTorque(Engine::Physics::ToBullet(torque)); }
}
void Engine::Components::RigidBody::AddImpulse(const glm::vec3& impulse)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyCentralImpulse(Engine::Physics::ToBullet(impulse)); }
}
void Engine::Components::RigidBody::SetWorldPosition(const glm::vec3& worldPosition)
{
    if (!Owner)
        return;

    if (EnsureBody())
    {
        btTransform transform = m_impl->body->getWorldTransform();
        transform.setOrigin(Engine::Physics::ToBullet(worldPosition));
        m_impl->body->setWorldTransform(transform);
        if (m_impl->motionState)
            m_impl->motionState->setWorldTransform(transform);
        m_impl->body->activate(true);
    }

    if (Owner->Parent)
    {
        const glm::mat4 parentWorld = Owner->Parent->transform.GetWorldMatrix();
        Owner->transform.position = glm::vec3(glm::inverse(parentWorld) *
            glm::vec4(worldPosition, 1.f));
    }
    else
    {
        Owner->transform.position = worldPosition;
    }

    m_impl->syncedWorldRevision = Owner->transform.GetWorldRevision();
}
void Engine::Components::RigidBody::SetWorldPose(const glm::vec3& worldPosition,
    const glm::quat& worldRotation)
{
    if (!Owner)
        return;

    const glm::quat normalizedWorldRotation = glm::normalize(worldRotation);
    if (EnsureBody())
    {
        btTransform transform;
        transform.setIdentity();
        transform.setOrigin(Engine::Physics::ToBullet(worldPosition));
        transform.setRotation(btQuaternion(normalizedWorldRotation.x,
            normalizedWorldRotation.y, normalizedWorldRotation.z,
            normalizedWorldRotation.w));
        m_impl->body->setWorldTransform(transform);
        if (m_impl->motionState)
            m_impl->motionState->setWorldTransform(transform);
        m_impl->body->activate(true);
    }

    glm::mat4 world = glm::mat4_cast(normalizedWorldRotation);
    world[3] = glm::vec4(worldPosition, 1.f);
    const glm::mat4 parentWorld = Owner->Parent
        ? Owner->Parent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parentWorld) * world;
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat localRotation;
    if (glm::decompose(local, scale, localRotation, translation, skew, perspective))
    {
        Owner->transform.position = translation;
        Owner->transform.rotation = glm::eulerAngles(glm::normalize(localRotation));
    }

    m_impl->syncedWorldRevision = Owner->transform.GetWorldRevision();
}
void Engine::Components::RigidBody::SetLinearVelocity(const glm::vec3& velocity)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->setLinearVelocity(Engine::Physics::ToBullet(velocity)); }
}
void Engine::Components::RigidBody::SetAngularVelocity(const glm::vec3& velocity)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->setAngularVelocity(Engine::Physics::ToBullet(velocity)); }
}
void Engine::Components::RigidBody::SetGravityDirection(const glm::vec3& direction)
{
    const float lengthSquared = glm::dot(direction, direction);
    gravityDirection = std::isfinite(lengthSquared) && lengthSquared > 1e-8f
        ? direction / std::sqrt(lengthSquared)
        : glm::vec3(0.f, -1.f, 0.f);
    if (m_impl && m_impl->body)
    {
        const glm::vec3 gravity = gravityDirection * (9.81f * gravityScale);
        m_impl->body->setGravity(useGravity ? Engine::Physics::ToBullet(gravity)
                                            : btVector3(0.f, 0.f, 0.f));
        m_impl->body->activate(true);
    }
}
glm::vec3 Engine::Components::RigidBody::GetLinearVelocity() const
{
    return m_impl && m_impl->body ? Engine::Physics::ToGlm(m_impl->body->getLinearVelocity()) : glm::vec3(0.f);
}
glm::vec3 Engine::Components::RigidBody::GetAngularVelocity() const
{
    return m_impl && m_impl->body ? Engine::Physics::ToGlm(m_impl->body->getAngularVelocity()) : glm::vec3(0.f);
}
glm::vec3 Engine::Components::RigidBody::GetGravityDirection() const
{
    const float lengthSquared = glm::dot(gravityDirection, gravityDirection);
    return std::isfinite(lengthSquared) && lengthSquared > 1e-8f
        ? gravityDirection / std::sqrt(lengthSquared)
        : glm::vec3(0.f, -1.f, 0.f);
}

float Engine::Components::RigidBody::ResolveFrictionFor(const RigidBody* other) const
{
    const auto matches = [other](const FrictionBehavior& behavior)
    {
        if (!behavior.enabled || !other)
            return false;
        const std::string mode = Engine::Physics::Lower(behavior.match);
        if (mode == "name")
            return other->Owner && other->Owner->name == behavior.target;
        if (mode == "identifier")
            return !behavior.target.empty() &&
                other->collisionIdentifier == behavior.target;
        return false;
    };
    for (const FrictionBehavior& behavior : frictionBehaviors)
        if (matches(behavior))
            return std::clamp(behavior.friction, 0.f, 1.f);
    for (const FrictionBehavior& behavior : frictionBehaviors)
        if (behavior.enabled && Engine::Physics::Lower(behavior.match) == "all")
            return std::clamp(behavior.friction, 0.f, 1.f);
    return std::clamp(friction, 0.f, 1.f);
}

Engine::Components::RigidBody::JsonValue
Engine::Components::RigidBody::Serialize() const
{
    JsonValue result = Component::Serialize();
    JsonValue behaviors = JsonValue::MakeArray();
    for (const FrictionBehavior& behavior : frictionBehaviors)
    {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("match", JsonValue(behavior.match));
        entry.Set("target", JsonValue(behavior.target));
        entry.Set("friction", JsonValue(behavior.friction));
        entry.Set("enabled", JsonValue(behavior.enabled));
        behaviors.Push(std::move(entry));
    }
    result.Set("frictionBehaviors", std::move(behaviors));
    return result;
}

void Engine::Components::RigidBody::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    if (!value.Has("frictionBehaviors"))
        return;
    frictionBehaviors.clear();
    const JsonValue& behaviors = value["frictionBehaviors"];
    if (!behaviors.IsArray())
        return;
    for (std::size_t index = 0; index < behaviors.ArraySize(); ++index)
    {
        const JsonValue& entry = behaviors.ArrayAt(index);
        if (!entry.IsObject())
            continue;
        FrictionBehavior behavior;
        if (entry.Has("match") && entry["match"].IsString())
            behavior.match = entry["match"].AsString();
        if (entry.Has("target") && entry["target"].IsString())
            behavior.target = entry["target"].AsString();
        if (entry.Has("friction") && entry["friction"].IsNumber())
            behavior.friction = std::clamp(entry["friction"].AsFloat(), 0.f, 1.f);
        if (entry.Has("enabled") && entry["enabled"].IsBool())
            behavior.enabled = entry["enabled"].AsBool();
        frictionBehaviors.push_back(std::move(behavior));
    }
}

bool Engine::Components::RigidBody::DrawProperties(
    ::Engine::Editor::IEditorUi& ui)
{
    bool changed = Component::DrawProperties(ui);
    ui.Separator();
    if (ui.PropertyGroupHeader("Friction Behaviors", false))
    {
        ui.Indent(12.f);
        ui.DisabledLabel("Specific Name/Identifier rules override the All fallback.");
        std::size_t removeIndex = frictionBehaviors.size();
        static const char* modes[] = { "All", "Name", "Identifier" };
        for (std::size_t index = 0; index < frictionBehaviors.size(); ++index)
        {
            FrictionBehavior& behavior = frictionBehaviors[index];
            ui.PushId(&behavior);
            int selected = Engine::Physics::Lower(behavior.match) == "name" ? 1
                : Engine::Physics::Lower(behavior.match) == "identifier" ? 2 : 0;
            if (ui.Combo("Match", &selected, modes, 3))
            {
                behavior.match = modes[selected];
                changed = true;
            }
            char target[256]{};
            std::strncpy(target, behavior.target.c_str(), sizeof(target) - 1);
            ui.BeginDisabled(selected == 0);
            if (ui.InputText("Target", target, sizeof(target)))
            {
                behavior.target = target;
                changed = true;
            }
            ui.EndDisabled();
            if (ui.DragFloat("Friction", &behavior.friction, 0.01f, 0.f, 1.f))
            {
                behavior.friction = std::clamp(behavior.friction, 0.f, 1.f);
                changed = true;
            }
            if (ui.Checkbox("Enabled", &behavior.enabled))
                changed = true;
            if (ui.Button("Remove"))
                removeIndex = index;
            ui.Separator();
            ui.PopId();
        }
        if (removeIndex < frictionBehaviors.size())
        {
            frictionBehaviors.erase(frictionBehaviors.begin() + removeIndex);
            changed = true;
        }
        if (ui.Button("Add Friction Behavior"))
        {
            frictionBehaviors.emplace_back();
            changed = true;
        }
        ui.Unindent(12.f);
    }
    if (changed)
        MarkConfigurationDirty();
    return changed;
}
}
