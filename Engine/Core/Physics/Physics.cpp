#include "Core/Physics/Physics.h"

#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <BulletSoftBody/btSoftBodyHelpers.h>
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#include <BulletSoftBody/btSoftRigidDynamicsWorld.h>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

namespace
{
struct PhysicsState
{
    std::unique_ptr<btSoftBodyRigidBodyCollisionConfiguration> collisionConfiguration =
        std::make_unique<btSoftBodyRigidBodyCollisionConfiguration>();
    std::unique_ptr<btCollisionDispatcher> dispatcher =
        std::make_unique<btCollisionDispatcher>(collisionConfiguration.get());
    std::unique_ptr<btDbvtBroadphase> broadphase = std::make_unique<btDbvtBroadphase>();
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver =
        std::make_unique<btSequentialImpulseConstraintSolver>();
    std::unique_ptr<btSoftRigidDynamicsWorld> world =
        std::make_unique<btSoftRigidDynamicsWorld>(dispatcher.get(), broadphase.get(),
            solver.get(), collisionConfiguration.get());
    btSoftBodyWorldInfo softBodyInfo{};

    PhysicsState()
    {
        world->setGravity(btVector3(0.f, -9.81f, 0.f));
        softBodyInfo.m_broadphase = broadphase.get();
        softBodyInfo.m_dispatcher = dispatcher.get();
        softBodyInfo.m_gravity = world->getGravity();
        softBodyInfo.m_sparsesdf.Initialize();
    }
};

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsDynamic(const RigidBody& body) { return Lower(body.bodyType) == "dynamic"; }
bool IsKinematic(const RigidBody& body) { return Lower(body.bodyType) == "kinematic"; }

btVector3 ToBullet(const glm::vec3& value)
{
    return { value.x, value.y, value.z };
}

glm::vec3 ToGlm(const btVector3& value)
{
    return { value.x(), value.y(), value.z() };
}

glm::vec3 WorldScale(const Object& object)
{
    const glm::mat4 world = object.transform.GetWorldMatrix();
    return {
        glm::length(glm::vec3(world[0])),
        glm::length(glm::vec3(world[1])),
        glm::length(glm::vec3(world[2]))
    };
}

btTransform ObjectWorldTransform(const Object& object)
{
    const glm::mat4 matrix = object.transform.GetWorldMatrix();
    glm::mat3 rotation;
    for (int column = 0; column < 3; ++column)
    {
        const glm::vec3 axis(matrix[column]);
        const float length = glm::length(axis);
        rotation[column] = length > 0.00001f ? axis / length : glm::vec3(column == 0, column == 1, column == 2);
    }
    const glm::quat quaternion = glm::normalize(glm::quat_cast(rotation));
    btTransform result;
    result.setIdentity();
    result.setOrigin(ToBullet(glm::vec3(matrix[3])));
    result.setRotation(btQuaternion(quaternion.x, quaternion.y, quaternion.z, quaternion.w));
    return result;
}

std::string ConfigurationSignature(const RigidBody& body)
{
    std::ostringstream output;
    output << JsonWrite(body.Serialize()) << '|';
    if (body.Owner)
    {
        const glm::vec3 scale = WorldScale(*body.Owner);
        output << scale.x << ',' << scale.y << ',' << scale.z;
        for (const Component* component : body.Owner->Components)
        {
            if (dynamic_cast<const Collider*>(component))
                output << '|' << JsonWrite(component->Serialize());
        }
        if (const Mesh* mesh = body.Owner->GetComponent<Mesh>())
            output << "|mesh:" << mesh->GetFilePath() << ':' << mesh->GetVertexCount();
    }
    return output.str();
}
}

struct Physics::Impl
{
    explicit Impl(Scene& owner) : scene(&owner), state(std::make_unique<PhysicsState>()) {}
    Scene* scene = nullptr;
    std::unique_ptr<PhysicsState> state;
};

Physics::Physics(Scene& scene) : m_impl(new Impl(scene)) {}
Physics::~Physics() { delete m_impl; }
void* Physics::GetInternalState() { return m_impl ? m_impl->state.get() : nullptr; }

namespace
{
PhysicsState& StateFor(Scene* scene)
{
    return *static_cast<PhysicsState*>(scene->GetPhysics().GetInternalState());
}
}

struct RigidBody::Impl
{
    Scene* scene = nullptr;
    std::string signature;
    std::unique_ptr<btCompoundShape> compound;
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btTriangleMesh>> triangleMeshes;
    std::unique_ptr<btDefaultMotionState> motionState;
    std::unique_ptr<btRigidBody> body;
};

RigidBody::RigidBody() : m_impl(new Impl())
{
    SetTypeName(COMPONENT_TYPE_NAME(RigidBody));
    singlecomponent = true;
    RegisterField("bodyType", bodyType);
    RegisterField("mass", mass);
    RegisterField("useGravity", useGravity);
    RegisterField("gravityScale", gravityScale);
    RegisterField("linearDamping", linearDamping);
    RegisterField("angularDamping", angularDamping);
    RegisterField("friction", friction);
    RegisterField("restitution", restitution);
    RegisterField("isTrigger", isTrigger);
    RegisterField("continuousCollision", continuousCollision);
    RegisterField("initialLinearVelocity", initialLinearVelocity);
    RegisterField("initialAngularVelocity", initialAngularVelocity);
    RegisterField("freezePositionX", freezePositionX);
    RegisterField("freezePositionY", freezePositionY);
    RegisterField("freezePositionZ", freezePositionZ);
    RegisterField("freezeRotationX", freezeRotationX);
    RegisterField("freezeRotationY", freezeRotationY);
    RegisterField("freezeRotationZ", freezeRotationZ);
    RegisterField("collisionLayer", collisionLayer);
    RegisterField("collisionMask", collisionMask);
}

RigidBody::~RigidBody()
{
    DestroyBody();
    delete m_impl;
}

bool RigidBody::EnsureBody()
{
    if (!Owner || !Owner->GetScene() || !Owner->IsEnabledInHierarchy())
    {
        DestroyBody();
        return false;
    }
    const std::string signature = ConfigurationSignature(*this);
    if (m_impl->body && m_impl->signature == signature)
        return true;
    DestroyBody();

    m_impl->scene = Owner->GetScene();
    m_impl->compound = std::make_unique<btCompoundShape>();
    const glm::vec3 scale = WorldScale(*Owner);

    for (Component* component : Owner->Components)
    {
        Collider* collider = dynamic_cast<Collider*>(component);
        if (!collider || !collider->collisionEnabled) continue;
        btTransform child;
        child.setIdentity();
        child.setOrigin(ToBullet(collider->center * scale));

        std::unique_ptr<btCollisionShape> shape;
        if (auto* primitive = dynamic_cast<PrimitiveObjectCollider*>(collider))
        {
            const std::string kind = Lower(primitive->shape);
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
                shape = std::make_unique<btBoxShape>(ToBullet(size * 0.5f));
        }
        else if (auto* meshCollider = dynamic_cast<MeshObjectCollider*>(collider))
        {
            std::unique_ptr<Mesh> loadedMesh;
            const Mesh* mesh = nullptr;
            if (meshCollider->meshReference.IsAssigned())
                mesh = ResolveComponentReference<Mesh>(Owner, meshCollider->meshReference);
            else if (!meshCollider->meshPath.empty())
            {
                loadedMesh = std::make_unique<Mesh>();
                try { loadedMesh->LoadFromFile(meshCollider->meshPath); }
                catch (...) { loadedMesh.reset(); }
                mesh = loadedMesh.get();
            }
            else
                mesh = Owner->GetComponent<Mesh>();
            if (!mesh || mesh->GetVertices().empty()) continue;

            const bool convex = meshCollider->convex || IsDynamic(*this);
            if (convex)
            {
                auto hull = std::make_unique<btConvexHullShape>();
                for (const Vertex& vertex : mesh->GetVertices())
                    hull->addPoint(btVector3(vertex.pos[0] * scale.x,
                        vertex.pos[1] * scale.y, vertex.pos[2] * scale.z), false);
                hull->recalcLocalAabb();
                shape = std::move(hull);
            }
            else
            {
                auto triangles = std::make_unique<btTriangleMesh>();
                const auto& vertices = mesh->GetVertices();
                for (std::size_t i = 0; i + 2 < vertices.size(); i += 3)
                {
                    const auto point = [&vertices, &scale](std::size_t index)
                    {
                        return btVector3(vertices[index].pos[0] * scale.x,
                            vertices[index].pos[1] * scale.y,
                            vertices[index].pos[2] * scale.z);
                    };
                    triangles->addTriangle(point(i), point(i + 1), point(i + 2));
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

    const bool dynamic = IsDynamic(*this);
    const btScalar bodyMass = dynamic ? std::max(0.001f, mass) : 0.f;
    btVector3 inertia(0.f, 0.f, 0.f);
    if (bodyMass > 0.f) m_impl->compound->calculateLocalInertia(bodyMass, inertia);
    const btTransform transform = ObjectWorldTransform(*Owner);
    m_impl->motionState = std::make_unique<btDefaultMotionState>(transform);
    btRigidBody::btRigidBodyConstructionInfo info(bodyMass, m_impl->motionState.get(),
        m_impl->compound.get(), inertia);
    m_impl->body = std::make_unique<btRigidBody>(info);
    m_impl->body->setUserPointer(this);
    if (IsKinematic(*this))
    {
        m_impl->body->setCollisionFlags(m_impl->body->getCollisionFlags() |
            btCollisionObject::CF_KINEMATIC_OBJECT);
        m_impl->body->setActivationState(DISABLE_DEACTIVATION);
    }
    if (isTrigger)
        m_impl->body->setCollisionFlags(m_impl->body->getCollisionFlags() |
            btCollisionObject::CF_NO_CONTACT_RESPONSE);
    ApplyBodySettings();
    m_impl->body->setLinearVelocity(ToBullet(initialLinearVelocity));
    m_impl->body->setAngularVelocity(ToBullet(initialAngularVelocity));
    StateFor(m_impl->scene).world->addRigidBody(m_impl->body.get(),
        static_cast<short>(collisionLayer), static_cast<short>(collisionMask));
    m_impl->signature = signature;
    return true;
}

void RigidBody::DestroyBody()
{
    if (!m_impl) return;
    if (m_impl->body && m_impl->scene)
    {
        StateFor(m_impl->scene).world->removeRigidBody(m_impl->body.get());
    }
    m_impl->body.reset();
    m_impl->motionState.reset();
    m_impl->compound.reset();
    m_impl->shapes.clear();
    m_impl->triangleMeshes.clear();
    m_impl->scene = nullptr;
    m_impl->signature.clear();
    m_isColliding = false;
    m_isGrounded = false;
}

void RigidBody::ApplyBodySettings()
{
    if (!m_impl || !m_impl->body) return;
    m_impl->body->setDamping(std::max(0.f, linearDamping), std::max(0.f, angularDamping));
    m_impl->body->setFriction(std::clamp(friction, 0.f, 1.f));
    m_impl->body->setRestitution(std::clamp(restitution, 0.f, 1.f));
    m_impl->body->setGravity(useGravity ? btVector3(0.f, -9.81f * gravityScale, 0.f)
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

void RigidBody::SyncBodyFromTransform()
{
    if (!m_impl || !m_impl->body || !Owner) return;
    const btTransform transform = ObjectWorldTransform(*Owner);
    m_impl->body->setWorldTransform(transform);
    if (m_impl->motionState) m_impl->motionState->setWorldTransform(transform);
    m_impl->body->activate(true);
}

void RigidBody::SyncTransformFromBody()
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
    }
}

void RigidBody::Start() { EnsureBody(); }
void RigidBody::Update()
{
    if (EnsureBody())
    {
        ApplyBodySettings();
        if (!IsDynamic(*this)) SyncBodyFromTransform();
    }
}
void RigidBody::Enabled() { EnsureBody(); }
void RigidBody::Disabled() { DestroyBody(); }
void RigidBody::OnDestroy() { DestroyBody(); }

void RigidBody::AddForce(const glm::vec3& force)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyCentralForce(ToBullet(force)); }
}
void RigidBody::AddTorque(const glm::vec3& torque)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyTorque(ToBullet(torque)); }
}
void RigidBody::AddImpulse(const glm::vec3& impulse)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->applyCentralImpulse(ToBullet(impulse)); }
}
void RigidBody::SetLinearVelocity(const glm::vec3& velocity)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->setLinearVelocity(ToBullet(velocity)); }
}
void RigidBody::SetAngularVelocity(const glm::vec3& velocity)
{
    if (EnsureBody()) { m_impl->body->activate(true); m_impl->body->setAngularVelocity(ToBullet(velocity)); }
}
glm::vec3 RigidBody::GetLinearVelocity() const
{
    return m_impl && m_impl->body ? ToGlm(m_impl->body->getLinearVelocity()) : glm::vec3(0.f);
}
glm::vec3 RigidBody::GetAngularVelocity() const
{
    return m_impl && m_impl->body ? ToGlm(m_impl->body->getAngularVelocity()) : glm::vec3(0.f);
}

struct Cloth::Impl
{
    Scene* scene = nullptr;
    Mesh* mesh = nullptr; // Render target owned by the object.
    std::string signature;
    std::vector<Vertex> originalVertices;
    std::vector<glm::vec3> nodeLocalPositions;
    std::vector<std::size_t> renderToNode;
    std::vector<glm::vec3> renderNodeOffsets;
    std::vector<int> pinnedNodes;
    std::unique_ptr<btSoftBody> softBody;
};

Cloth::Cloth() : m_impl(new Impl())
{
    SetTypeName(COMPONENT_TYPE_NAME(Cloth));
    singlecomponent = true;
    RegisterField("simulationMeshReference", simulationMeshReference);
    RegisterField("renderMeshReference", renderMeshReference);
    RegisterField("meshPath", meshPath);
    RegisterField("mass", mass);
    RegisterField("linearStiffness", linearStiffness);
    RegisterField("bendingStiffness", bendingStiffness);
    RegisterField("damping", damping);
    RegisterField("drag", drag);
    RegisterField("friction", friction);
    RegisterField("gravityScale", gravityScale);
    RegisterField("collisionMargin", collisionMargin);
    RegisterField("solverIterations", solverIterations);
    RegisterField("selfCollision", selfCollision);
    RegisterField("pinMode", pinMode);
    RegisterField("pinThreshold", pinThreshold);
    RegisterField("windVelocity", windVelocity);
    RegisterField("windStrength", windStrength);
}

Cloth::~Cloth()
{
    DestroySoftBody(true);
    delete m_impl;
}

bool Cloth::IsSimulating() const
{
    return m_impl && m_impl->softBody != nullptr;
}

bool Cloth::EnsureSoftBody()
{
    if (!Owner || !Owner->GetScene() || !Owner->IsEnabledInHierarchy())
    {
        DestroySoftBody(true);
        return false;
    }
    Mesh* mesh = renderMeshReference.IsAssigned()
        ? ResolveComponentReference<Mesh>(Owner, renderMeshReference)
        : Owner->GetComponent<Mesh>();
    if (!mesh || mesh->GetVertices().size() < 3) return false;

    std::unique_ptr<Mesh> loadedSimulationMesh;
    const Mesh* simulationMesh = simulationMeshReference.IsAssigned()
        ? ResolveComponentReference<Mesh>(Owner, simulationMeshReference) : nullptr;
    if (simulationMeshReference.IsAssigned() && !simulationMesh) return false;
    if (!simulationMesh && !meshPath.empty())
    {
        loadedSimulationMesh = std::make_unique<Mesh>();
        try { loadedSimulationMesh->LoadFromFile(meshPath); }
        catch (...) { return false; }
        simulationMesh = loadedSimulationMesh.get();
    }
    if (!simulationMesh) simulationMesh = mesh;
    if (simulationMesh->GetVertices().size() < 3) return false;

    const glm::vec3 scale = WorldScale(*Owner);
    std::ostringstream signatureBuilder;
    signatureBuilder << JsonWrite(Serialize()) << "|render:" << mesh->GetFilePath() << ':'
        << mesh->GetVertexCount() << "|simulation:" << simulationMesh->GetFilePath() << ':'
        << simulationMesh->GetVertexCount() << '|' << scale.x << ',' << scale.y << ',' << scale.z;
    const std::string signature = signatureBuilder.str();
    if (m_impl->softBody && m_impl->signature == signature && m_impl->mesh == mesh)
        return true;
    DestroySoftBody(true);

    m_impl->scene = Owner->GetScene();
    m_impl->mesh = mesh;
    m_impl->signature = signature;
    m_impl->originalVertices = mesh->GetVertices();

    struct PositionKey
    {
        long long x, y, z;
        bool operator==(const PositionKey& other) const
        { return x == other.x && y == other.y && z == other.z; }
    };
    struct PositionHash
    {
        std::size_t operator()(const PositionKey& key) const
        {
            std::size_t hash = std::hash<long long>{}(key.x);
            hash ^= std::hash<long long>{}(key.y) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<long long>{}(key.z) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    std::unordered_map<PositionKey, std::size_t, PositionHash> welded;
    const std::vector<Vertex>& simulationVertices = simulationMesh->GetVertices();
    std::vector<std::size_t> simulationToNode(simulationVertices.size());
    constexpr double precision = 100000.0;
    for (std::size_t index = 0; index < simulationVertices.size(); ++index)
    {
        const Vertex& vertex = simulationVertices[index];
        const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const PositionKey key { std::llround(position.x * precision),
            std::llround(position.y * precision), std::llround(position.z * precision) };
        auto [found, inserted] = welded.emplace(key, m_impl->nodeLocalPositions.size());
        if (inserted) m_impl->nodeLocalPositions.push_back(position);
        simulationToNode[index] = found->second;
    }

    // Bind every render vertex to its nearest simulation node. Keeping its
    // rest-space offset allows a lower-detail or otherwise different cloth
    // cage to drive the visible mesh without snapping it onto the cage.
    m_impl->renderToNode.resize(m_impl->originalVertices.size());
    m_impl->renderNodeOffsets.resize(m_impl->originalVertices.size());
    const glm::mat4 clothWorld = Owner->transform.GetWorldMatrix();
    const glm::mat4 renderToCloth = glm::inverse(clothWorld) *
        (mesh->Owner ? mesh->Owner->transform.GetWorldMatrix() : clothWorld);
    for (std::size_t index = 0; index < m_impl->originalVertices.size(); ++index)
    {
        const Vertex& vertex = m_impl->originalVertices[index];
        const glm::vec3 renderPosition(renderToCloth * glm::vec4(
            vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f));
        std::size_t nearest = 0;
        float nearestDistance = std::numeric_limits<float>::max();
        for (std::size_t node = 0; node < m_impl->nodeLocalPositions.size(); ++node)
        {
            const glm::vec3 delta = renderPosition - m_impl->nodeLocalPositions[node];
            const float distance = glm::dot(delta, delta);
            if (distance < nearestDistance)
            {
                nearest = node;
                nearestDistance = distance;
            }
        }
        m_impl->renderToNode[index] = nearest;
        m_impl->renderNodeOffsets[index] = renderPosition - m_impl->nodeLocalPositions[nearest];
    }

    std::vector<btScalar> worldVertices;
    worldVertices.reserve(m_impl->nodeLocalPositions.size() * 3);
    const glm::mat4 objectWorld = clothWorld;
    for (const glm::vec3& local : m_impl->nodeLocalPositions)
    {
        const glm::vec3 world(objectWorld * glm::vec4(local, 1.f));
        worldVertices.push_back(world.x);
        worldVertices.push_back(world.y);
        worldVertices.push_back(world.z);
    }
    std::vector<int> triangles;
    triangles.reserve(simulationToNode.size());
    for (std::size_t node : simulationToNode)
        triangles.push_back(static_cast<int>(node));

    PhysicsState& physics = StateFor(m_impl->scene);
    m_impl->softBody.reset(btSoftBodyHelpers::CreateFromTriMesh(
        physics.softBodyInfo, worldVertices.data(), triangles.data(),
        static_cast<int>(triangles.size() / 3), false));
    if (!m_impl->softBody)
    {
        DestroySoftBody(true);
        return false;
    }

    btSoftBody* softBody = m_impl->softBody.get();
    // Rigid-body contact reporting treats non-null user pointers as RigidBody.
    // Cloth collision response is handled internally and needs no callback tag.
    softBody->setUserPointer(nullptr);
    btSoftBody::Material* material = softBody->appendMaterial();
    material->m_kLST = std::clamp(linearStiffness, 0.f, 1.f);
    material->m_kAST = std::clamp(bendingStiffness, 0.f, 1.f);
    if (bendingStiffness > 0.f) softBody->generateBendingConstraints(2, material);
    softBody->m_cfg.kDP = std::clamp(damping, 0.f, 1.f);
    softBody->m_cfg.kDG = std::max(0.f, drag);
    softBody->m_cfg.kDF = std::clamp(friction, 0.f, 1.f);
    softBody->m_cfg.piterations = std::max(1, solverIterations);
    softBody->m_cfg.viterations = std::max(1, solverIterations / 2);
    softBody->m_cfg.collisions = btSoftBody::fCollision::SDF_RS;
    if (selfCollision) softBody->m_cfg.collisions |= btSoftBody::fCollision::VF_SS;
    softBody->getCollisionShape()->setMargin(std::max(0.0001f, collisionMargin));
    softBody->setTotalMass(std::max(0.001f, mass), true);

    const std::string pin = Lower(pinMode);
    if (pin != "none" && !m_impl->nodeLocalPositions.empty())
    {
        float boundary = pin == "bottom" ? std::numeric_limits<float>::max()
            : pin == "left" ? std::numeric_limits<float>::max()
            : -std::numeric_limits<float>::max();
        for (const glm::vec3& position : m_impl->nodeLocalPositions)
        {
            const float value = (pin == "left" || pin == "right") ? position.x : position.y;
            boundary = (pin == "bottom" || pin == "left")
                ? std::min(boundary, value) : std::max(boundary, value);
        }
        const float threshold = std::max(0.f, pinThreshold);
        for (std::size_t index = 0; index < m_impl->nodeLocalPositions.size(); ++index)
        {
            const glm::vec3& position = m_impl->nodeLocalPositions[index];
            const float value = (pin == "left" || pin == "right") ? position.x : position.y;
            if (std::abs(value - boundary) <= threshold)
            {
                softBody->setMass(static_cast<int>(index), 0.f);
                m_impl->pinnedNodes.push_back(static_cast<int>(index));
            }
        }
    }

    physics.world->addSoftBody(softBody);
    return true;
}

void Cloth::DestroySoftBody(bool restoreMesh)
{
    if (!m_impl) return;
    if (m_impl->softBody && m_impl->scene)
    {
        StateFor(m_impl->scene).world->removeSoftBody(m_impl->softBody.get());
    }
    m_impl->softBody.reset();
    if (restoreMesh && m_impl->mesh && !m_impl->originalVertices.empty())
        m_impl->mesh->SetDeformedVertices(m_impl->originalVertices);
    m_impl->scene = nullptr;
    m_impl->mesh = nullptr;
    m_impl->signature.clear();
    m_impl->originalVertices.clear();
    m_impl->nodeLocalPositions.clear();
    m_impl->renderToNode.clear();
    m_impl->renderNodeOffsets.clear();
    m_impl->pinnedNodes.clear();
}

void Cloth::UpdatePinnedNodes()
{
    if (!m_impl || !m_impl->softBody || !Owner) return;
    const glm::mat4 world = Owner->transform.GetWorldMatrix();
    for (int index : m_impl->pinnedNodes)
    {
        const glm::vec3 position(world * glm::vec4(m_impl->nodeLocalPositions[index], 1.f));
        auto& node = m_impl->softBody->m_nodes[index];
        node.m_x = ToBullet(position);
        node.m_q = node.m_x;
        node.m_v.setZero();
    }
}

void Cloth::ApplyForces()
{
    if (!m_impl || !m_impl->softBody) return;
    btSoftBody* softBody = m_impl->softBody.get();
    if (windStrength > 0.f && glm::length(windVelocity) > 0.0001f)
        softBody->addForce(ToBullet(windVelocity * windStrength));
    if (gravityScale != 1.f)
    {
        for (int index = 0; index < softBody->m_nodes.size(); ++index)
        {
            const btScalar inverseMass = softBody->m_nodes[index].m_im;
            if (inverseMass > 0.f)
                softBody->addForce(btVector3(0.f,
                    -9.81f * (gravityScale - 1.f) / inverseMass, 0.f), index);
        }
    }
}

void Cloth::SyncMeshFromSoftBody()
{
    if (!m_impl || !m_impl->softBody || !m_impl->mesh || !Owner) return;
    std::vector<Vertex> deformed = m_impl->originalVertices;
    const glm::mat4 clothWorld = Owner->transform.GetWorldMatrix();
    const glm::mat4 inverseWorld = glm::inverse(clothWorld);
    const glm::mat4 clothToRender = glm::inverse(m_impl->mesh->Owner
        ? m_impl->mesh->Owner->transform.GetWorldMatrix() : clothWorld) * clothWorld;
    for (std::size_t index = 0; index < deformed.size(); ++index)
    {
        const btVector3& world = m_impl->softBody->m_nodes[
            static_cast<int>(m_impl->renderToNode[index])].m_x;
        const glm::vec3 local(inverseWorld * glm::vec4(world.x(), world.y(), world.z(), 1.f));
        const glm::vec3 renderPosition(clothToRender * glm::vec4(
            local + m_impl->renderNodeOffsets[index], 1.f));
        deformed[index].pos[0] = renderPosition.x;
        deformed[index].pos[1] = renderPosition.y;
        deformed[index].pos[2] = renderPosition.z;
    }
    for (std::size_t index = 0; index + 2 < deformed.size(); index += 3)
    {
        const glm::vec3 first(deformed[index].pos[0], deformed[index].pos[1], deformed[index].pos[2]);
        const glm::vec3 second(deformed[index + 1].pos[0], deformed[index + 1].pos[1], deformed[index + 1].pos[2]);
        const glm::vec3 third(deformed[index + 2].pos[0], deformed[index + 2].pos[1], deformed[index + 2].pos[2]);
        const glm::vec3 cross = glm::cross(second - first, third - first);
        const glm::vec3 normal = glm::length(cross) > 0.000001f
            ? glm::normalize(cross) : glm::vec3(0.f, 1.f, 0.f);
        for (std::size_t vertex = index; vertex < index + 3; ++vertex)
        {
            deformed[vertex].normal[0] = normal.x;
            deformed[vertex].normal[1] = normal.y;
            deformed[vertex].normal[2] = normal.z;
        }
    }
    m_impl->mesh->SetDeformedVertices(deformed);
}

void Cloth::ResetSimulation()
{
    DestroySoftBody(true);
    EnsureSoftBody();
}
void Cloth::Start() { EnsureSoftBody(); }
void Cloth::Update() { EnsureSoftBody(); }
void Cloth::Enabled() { EnsureSoftBody(); }
void Cloth::Disabled() { DestroySoftBody(true); }
void Cloth::OnDestroy() { DestroySoftBody(true); }

void Physics::Step(float deltaTime)
{
    Scene& scene = *m_impl->scene;
    std::vector<RigidBody*> bodies;
    std::vector<Cloth*> clothBodies;
    for (const auto& object : scene.GetObjects())
        for (Component* component : object->Components)
            if (auto* body = dynamic_cast<RigidBody*>(component))
            {
                bodies.push_back(body);
                body->m_isColliding = false;
                body->m_isGrounded = false;
                if (body->EnsureBody() && !IsDynamic(*body)) body->SyncBodyFromTransform();
            }
            else if (auto* cloth = dynamic_cast<Cloth*>(component))
            {
                clothBodies.push_back(cloth);
                if (cloth->EnsureSoftBody())
                {
                    cloth->UpdatePinnedNodes();
                    cloth->ApplyForces();
                }
            }
    if (bodies.empty() && clothBodies.empty()) return;

    PhysicsState& physics = *m_impl->state;
    physics.world->stepSimulation(std::clamp(deltaTime, 0.f, 0.1f), 6, 1.f / 60.f);

    for (RigidBody* body : bodies)
        if (body->m_impl->body && IsDynamic(*body)) body->SyncTransformFromBody();
    for (Cloth* cloth : clothBodies)
        if (cloth->IsSimulating()) cloth->SyncMeshFromSoftBody();

    const int manifoldCount = physics.dispatcher->getNumManifolds();
    for (int index = 0; index < manifoldCount; ++index)
    {
        btPersistentManifold* manifold = physics.dispatcher->getManifoldByIndexInternal(index);
        auto* first = static_cast<RigidBody*>(manifold->getBody0()->getUserPointer());
        auto* second = static_cast<RigidBody*>(manifold->getBody1()->getUserPointer());
        for (int contact = 0; contact < manifold->getNumContacts(); ++contact)
        {
            const btManifoldPoint& point = manifold->getContactPoint(contact);
            if (point.getDistance() > 0.f) continue;
            if (first)
            {
                first->m_isColliding = true;
                if (point.m_normalWorldOnB.y() > 0.5f) first->m_isGrounded = true;
            }
            if (second)
            {
                second->m_isColliding = true;
                if (-point.m_normalWorldOnB.y() > 0.5f) second->m_isGrounded = true;
            }
        }
    }
}

void Physics::Reset()
{
    Scene& scene = *m_impl->scene;
    for (const auto& object : scene.GetObjects())
        for (Component* component : object->Components)
            if (auto* body = dynamic_cast<RigidBody*>(component)) body->DestroyBody();
            else if (auto* cloth = dynamic_cast<Cloth*>(component)) cloth->DestroySoftBody(true);
    m_impl->state = std::make_unique<PhysicsState>();
}
