#include "Core/Physics/Physics.h"
#include "Core/Physics/IPhysicsAdapter.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Memory/CacheStore.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Internal/PhysicsInternal.h"
#include "Core/Physics/Cloth/ClothSimulationMeshCache.h"
#include <BulletSoftBody/btSoftBodyHelpers.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace Engine::Components
{
struct Engine::Components::Cloth::Impl final : Engine::Physics::PhysicsComponentState
{
    Engine::Scene::Scene* scene = nullptr;
    Engine::Components::Mesh* mesh = nullptr; // Render target owned by the object.
    const Engine::Components::Mesh* simulationMeshSource = nullptr;
    std::shared_ptr<const Engine::Components::Mesh> cachedSimulationMesh;
    std::string simulationMeshPath;
    uint64_t configurationRevision = 0;
    uint64_t renderMeshRevision = 0;
    uint64_t simulationMeshRevision = 0;
    glm::vec3 worldScale{};
    std::vector<Engine::Model::AnimationVertex> originalVertices;
    std::vector<glm::vec3> nodeLocalPositions;
    std::vector<std::size_t> renderToNode;
    std::vector<glm::vec3> renderNodeOffsets;
    std::vector<int> pinnedNodes;
    struct AnchoredNode
    {
        int index = -1;
        Engine::Core::Object* body = nullptr;
        bool nearMode = false;
        void* nativeBody = nullptr;
        uint64_t bodyGeneration = 0;
    };
    std::vector<AnchoredNode> anchoredNodes;
    std::unique_ptr<btSoftBody> softBody;
    glm::vec3 morphWorldNormal { 0.f, 1.f, 0.f };
    glm::vec3 morphLocalCenter { 0.f };
    float morphAmount = 0.f;
};

std::unique_ptr<Engine::Physics::PhysicsComponentState>
Engine::Components::Cloth::MakeBulletState()
{
    return std::make_unique<Impl>();
}

void Engine::Components::Cloth::BindBulletState(
    Engine::Physics::PhysicsComponentState* state)
{
    m_impl = static_cast<Impl*>(state);
}

Engine::Components::Cloth::Cloth()
{
    SetTypeName("Cloth");
    singlecomponent = true;
    RegisterField("simulationMeshReference", simulationMeshReference);
    RegisterField("renderMeshReference", renderMeshReference);
    RegisterField("meshPath", meshPath);
    RegisterField("topology", topology);
    RegisterField("mass", mass);
    RegisterField("linearStiffness", linearStiffness);
    RegisterField("bendingStiffness", bendingStiffness);
    RegisterField("damping", damping);
    RegisterField("drag", drag);
    RegisterField("friction", friction);
    RegisterField("gravityScale", gravityScale);
    RegisterField("collisionMargin", collisionMargin);
    RegisterField("solverIterations", solverIterations);
    RegisterField("clusterCount", clusterCount);
    RegisterField("clusterIterations", clusterIterations);
    RegisterField("clusterHardness", clusterHardness);
    RegisterField("clusterImpulseSplit", clusterImpulseSplit);
    RegisterField("collisionLayer", collisionLayer, "Filtering", false);
    RegisterField("collisionMask", collisionMask, "Filtering", false);
    RegisterField("selfCollision", selfCollision);
    RegisterField("collisionMorph", collisionMorph);
    RegisterField("collisionMorphStrength", collisionMorphStrength);
    RegisterField("collisionMorphMaximum", collisionMorphMaximum);
    RegisterField("collisionMorphRecovery", collisionMorphRecovery);
    RegisterField("collisionMorphExpansion", collisionMorphExpansion);
    RegisterField("pinMode", pinMode);
    RegisterField("pinThreshold", pinThreshold);
    RegisterField("windVelocity", windVelocity);
    RegisterField("windStrength", windStrength);
}

Engine::Components::Cloth::JsonValue Engine::Components::Cloth::Serialize() const
{
    JsonValue value = Component::Serialize();
    JsonValue pins = JsonValue::MakeArray();
    const auto vectorValue = [](const glm::vec3& position)
    {
        return JsonValue::MakeArray().Push(JsonValue(position.x))
            .Push(JsonValue(position.y)).Push(JsonValue(position.z));
    };
    for (const glm::vec3& position : explicitPinPositions)
        pins.Push(vectorValue(position));
    value.Set("explicitPinPositions", std::move(pins));
    JsonValue bindings = JsonValue::MakeArray();
    for (const Anchor& anchor : anchors)
        bindings.Push(JsonValue::MakeObject()
            .Set("vertexPosition", vectorValue(anchor.vertexPosition))
            .Set("bodyName", JsonValue(anchor.bodyName))
            .Set("nearMode", JsonValue(anchor.nearMode)));
    return value.Set("anchors", std::move(bindings));
}

void Engine::Components::Cloth::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    const auto readPosition = [](const JsonValue& source)
    {
        return source.ArraySize() >= 3
            ? glm::vec3(source.ArrayAt(0).AsFloat(),
                source.ArrayAt(1).AsFloat(), source.ArrayAt(2).AsFloat())
            : glm::vec3(0.f);
    };
    explicitPinPositions.clear();
    for (size_t i = 0; i < value["explicitPinPositions"].ArraySize(); ++i)
        explicitPinPositions.push_back(readPosition(
            value["explicitPinPositions"].ArrayAt(i)));
    anchors.clear();
    for (size_t i = 0; i < value["anchors"].ArraySize(); ++i)
    {
        const auto& source = value["anchors"].ArrayAt(i);
        anchors.push_back({ readPosition(source["vertexPosition"]),
            source["bodyName"].AsString(), source["nearMode"].AsBool() });
    }
}

Engine::Components::Cloth::~Cloth()
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().DestroyCloth(*this);
    else DestroySoftBody(true);
}

bool Engine::Components::Cloth::IsSimulating() const
{
    if (Owner && Owner->GetScene())
        return Owner->GetScene()->GetPhysics().IsClothSimulating(*this);
    return NativeIsSimulating();
}

int Engine::Components::Cloth::GetActiveClusterCount() const
{
    return m_impl && m_impl->softBody
        ? m_impl->softBody->m_clusters.size() : 0;
}

int Engine::Components::Cloth::GetActiveAnchorCount() const
{
    return m_impl && m_impl->softBody
        ? m_impl->softBody->m_anchors.size() : 0;
}

int Engine::Components::Cloth::GetActiveLinkCount() const
{
    return m_impl && m_impl->softBody
        ? m_impl->softBody->m_links.size() : 0;
}

int Engine::Components::Cloth::GetActiveFaceCount() const
{
    return m_impl && m_impl->softBody
        ? m_impl->softBody->m_faces.size() : 0;
}

bool Engine::Components::Cloth::NativeIsSimulating() const
{
    return m_impl && (m_impl->softBody != nullptr ||
        (collisionMorph && m_impl->mesh != nullptr));
}

bool Engine::Components::Cloth::EnsureCollisionMorph()
{
    if (!Owner || !Owner->GetScene() || !Owner->IsEnabledInHierarchy())
    {
        DestroySoftBody(true);
        return false;
    }
    Engine::Components::Mesh* mesh = renderMeshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(
            Owner, renderMeshReference)
        : Owner->GetComponent<Engine::Components::Mesh>();
    if (!mesh || mesh->GetVertices().size() < 3)
    {
        DestroySoftBody(true);
        return false;
    }
    const glm::vec3 scale = Engine::Physics::WorldScale(*Owner);
    if (!m_impl->softBody && m_impl->mesh == mesh &&
        !m_impl->originalVertices.empty() &&
        m_impl->configurationRevision == GetConfigurationRevision() &&
        m_impl->renderMeshRevision == mesh->GetConfigurationRevision() &&
        Engine::Physics::SameVector(m_impl->worldScale, scale))
        return true;

    DestroySoftBody(true);
    m_impl->scene = Owner->GetScene();
    m_impl->mesh = mesh;
    m_impl->originalVertices = mesh->GetVertices();
    glm::vec3 boundsMinimum(std::numeric_limits<float>::max());
    glm::vec3 boundsMaximum(std::numeric_limits<float>::lowest());
    for (const Engine::Model::AnimationVertex& vertex : m_impl->originalVertices)
    {
        const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        boundsMinimum = glm::min(boundsMinimum, position);
        boundsMaximum = glm::max(boundsMaximum, position);
    }
    m_impl->morphLocalCenter = (boundsMinimum + boundsMaximum) * 0.5f;
    m_impl->configurationRevision = GetConfigurationRevision();
    m_impl->renderMeshRevision = mesh->GetConfigurationRevision();
    m_impl->worldScale = scale;
    return true;
}

bool Engine::Components::Cloth::EnsureSoftBody()
{
    if (!Owner || !Owner->GetScene() || !Owner->IsEnabledInHierarchy())
    {
        DestroySoftBody(true);
        return false;
    }
    Engine::Components::Mesh* mesh = renderMeshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(Owner, renderMeshReference)
        : Owner->GetComponent<Engine::Components::Mesh>();
    if (!mesh || mesh->GetVertices().size() < 3) return false;

    const Engine::Components::Mesh* referencedSimulationMesh = simulationMeshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Engine::Components::Mesh>(Owner, simulationMeshReference) : nullptr;
    if (simulationMeshReference.IsAssigned() && !referencedSimulationMesh) return false;
    const bool usesFileSimulationMesh =
        !referencedSimulationMesh && !meshPath.empty();
    const std::string simulationMeshPathKey = usesFileSimulationMesh
        ? Engine::Memory::CacheStore::PathKey(meshPath) : std::string{};
    const Engine::Components::Mesh* persistentSimulationMesh =
        referencedSimulationMesh ? referencedSimulationMesh
        : usesFileSimulationMesh ? m_impl->cachedSimulationMesh.get() : mesh;
    const glm::vec3 scale = Engine::Physics::WorldScale(*Owner);
    if (m_impl->softBody &&
        m_impl->configurationRevision == GetConfigurationRevision() &&
        m_impl->mesh == mesh &&
        m_impl->renderMeshRevision == mesh->GetConfigurationRevision() &&
        m_impl->simulationMeshSource == persistentSimulationMesh &&
        m_impl->simulationMeshPath == simulationMeshPathKey &&
        m_impl->simulationMeshRevision == (persistentSimulationMesh
            ? persistentSimulationMesh->GetConfigurationRevision() : 0) &&
        Engine::Physics::SameVector(m_impl->worldScale, scale))
    {
        return true;
    }
    DestroySoftBody(true);

    const Engine::Components::Mesh* simulationMesh = referencedSimulationMesh;
    if (usesFileSimulationMesh)
    {
        m_impl->cachedSimulationMesh =
            Engine::Physics::ClothSimulationMeshCache::Acquire(meshPath);
        if (!m_impl->cachedSimulationMesh)
            return false;
        simulationMesh = m_impl->cachedSimulationMesh.get();
    }
    if (!simulationMesh) simulationMesh = mesh;
    if (simulationMesh->GetVertices().size() < 3) return false;

    m_impl->scene = Owner->GetScene();
    m_impl->mesh = mesh;
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
    const std::vector<Engine::Model::AnimationVertex>& simulationVertices = simulationMesh->GetVertices();
    std::vector<std::size_t> simulationToNode(simulationVertices.size());
    constexpr double precision = 100000.0;
    for (std::size_t index = 0; index < simulationVertices.size(); ++index)
    {
        const Engine::Model::AnimationVertex& vertex = simulationVertices[index];
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
    const bool sameMesh = simulationMesh == mesh &&
        simulationToNode.size() == m_impl->originalVertices.size();
    for (std::size_t index = 0; index < m_impl->originalVertices.size(); ++index)
    {
        if (sameMesh)
        {
            m_impl->renderToNode[index] = simulationToNode[index];
            m_impl->renderNodeOffsets[index] = glm::vec3(0.f);
            continue;
        }
        const Engine::Model::AnimationVertex& vertex = m_impl->originalVertices[index];
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

    Engine::Physics::PhysicsWorldState& physics = Engine::Physics::StateFor(m_impl->scene);
    if (Engine::Physics::Lower(topology) == "rope")
    {
        // A PMX rope still references a triangle material. Use its welded
        // triangle edges to connect every vertex without retaining faces or
        // adding diagonal cycles that would stiffen a narrow strip.
        struct Edge { int first, second; float lengthSquared; };
        std::vector<Edge> edges;
        for (size_t face = 0; face + 2 < triangles.size(); face += 3)
            for (int side = 0; side < 3; ++side)
            {
                const int first = triangles[face + side];
                const int second = triangles[face + (side + 1) % 3];
                if (first == second) continue;
                const glm::vec3 delta = m_impl->nodeLocalPositions[first] -
                    m_impl->nodeLocalPositions[second];
                edges.push_back({ std::min(first, second), std::max(first, second),
                    glm::dot(delta, delta) });
            }
        std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b)
        {
            if (a.lengthSquared != b.lengthSquared)
                return a.lengthSquared < b.lengthSquared;
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
        std::vector<int> parent(m_impl->nodeLocalPositions.size());
        std::iota(parent.begin(), parent.end(), 0);
        const auto root = [&](int node)
        {
            while (parent[node] != node)
            {
                parent[node] = parent[parent[node]];
                node = parent[node];
            }
            return node;
        };
        std::vector<btVector3> positions;
        positions.reserve(parent.size());
        for (size_t index = 0; index < parent.size(); ++index)
            positions.emplace_back(worldVertices[index * 3],
                worldVertices[index * 3 + 1], worldVertices[index * 3 + 2]);
        std::vector<btScalar> nodeMass(parent.size(), btScalar(1));
        m_impl->softBody = std::make_unique<btSoftBody>(&physics.softBodyInfo,
            static_cast<int>(positions.size()), positions.data(), nodeMass.data());
        for (const Edge& edge : edges)
        {
            const int firstRoot = root(edge.first);
            const int secondRoot = root(edge.second);
            if (firstRoot == secondRoot) continue;
            m_impl->softBody->appendLink(edge.first, edge.second);
            parent[firstRoot] = secondRoot;
        }
    }
    else
        m_impl->softBody.reset(btSoftBodyHelpers::CreateFromTriMesh(
            physics.softBodyInfo, worldVertices.data(), triangles.data(),
            static_cast<int>(triangles.size() / 3), false));
    if (!m_impl->softBody)
    {
        DestroySoftBody(true);
        return false;
    }

    btSoftBody* softBody = m_impl->softBody.get();
    // Rigid-body contact reporting treats non-null user pointers as Engine::Components::RigidBody.
    // Engine::Components::Cloth collision response is handled internally and needs no callback tag.
    softBody->setUserPointer(nullptr);
    btSoftBody::Material* material = softBody->appendMaterial();
    material->m_kLST = std::clamp(linearStiffness, 0.f, 1.f);
    material->m_kAST = std::clamp(bendingStiffness, 0.f, 1.f);
    if (bendingStiffness > 0.f && Engine::Physics::Lower(topology) != "rope")
        softBody->generateBendingConstraints(2, material);
    softBody->m_cfg.kDP = std::clamp(damping, 0.f, 1.f);
    softBody->m_cfg.kDG = std::max(0.f, drag);
    softBody->m_cfg.kDF = std::clamp(friction, 0.f, 1.f);
    softBody->m_cfg.piterations = std::max(1, solverIterations);
    softBody->m_cfg.viterations = std::max(1, solverIterations / 2);
    softBody->m_cfg.citerations = std::max(1, clusterIterations);
    softBody->m_cfg.kSRHR_CL = std::clamp(clusterHardness.x, 0.f, 1.f);
    softBody->m_cfg.kSKHR_CL = std::clamp(clusterHardness.y, 0.f, 1.f);
    softBody->m_cfg.kSSHR_CL = std::clamp(clusterHardness.z, 0.f, 1.f);
    softBody->m_cfg.kSR_SPLT_CL = std::clamp(clusterImpulseSplit.x, 0.f, 1.f);
    softBody->m_cfg.kSK_SPLT_CL = std::clamp(clusterImpulseSplit.y, 0.f, 1.f);
    softBody->m_cfg.kSS_SPLT_CL = std::clamp(clusterImpulseSplit.z, 0.f, 1.f);
    softBody->m_cfg.collisions = btSoftBody::fCollision::SDF_RS;
    if (clusterCount > 0 && Engine::Physics::Lower(topology) != "rope")
    {
        softBody->generateClusters(std::min(clusterCount,
            static_cast<int>(m_impl->nodeLocalPositions.size())));
        softBody->m_cfg.collisions |= btSoftBody::fCollision::CL_RS;
    }
    if (selfCollision) softBody->m_cfg.collisions |= btSoftBody::fCollision::VF_SS;
    softBody->getCollisionShape()->setMargin(std::max(0.0001f, collisionMargin));
    // Ropes have links but no faces; face-area mass distribution divides by
    // zero and poisons Bullet's constraint matrices with NaNs.
    softBody->setTotalMass(std::max(0.001f, mass),
        Engine::Physics::Lower(topology) != "rope");

    const std::string pin = Engine::Physics::Lower(pinMode);
    if ((pin == "top" || pin == "bottom" || pin == "left" ||
            pin == "right") && !m_impl->nodeLocalPositions.empty())
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

    const auto nearestNode = [&](const glm::vec3& position)
    {
        int nearest = -1;
        float distance = std::numeric_limits<float>::max();
        for (size_t node = 0; node < m_impl->nodeLocalPositions.size(); ++node)
        {
            const glm::vec3 delta = m_impl->nodeLocalPositions[node] - position;
            const float candidate = glm::dot(delta, delta);
            if (candidate < distance)
            {
                distance = candidate;
                nearest = static_cast<int>(node);
            }
        }
        return nearest;
    };
    for (const glm::vec3& position : explicitPinPositions)
        if (const int node = nearestNode(position); node >= 0)
        {
            softBody->setMass(node, 0.f);
            m_impl->pinnedNodes.push_back(node);
        }
    for (const Anchor& anchor : anchors)
    {
        const int node = nearestNode(anchor.vertexPosition);
        Engine::Core::Object* body = FindObjectInSceneByName(anchor.bodyName);
        if (node < 0 || !body || !body->GetComponent<RigidBody>()) continue;
        if (std::find(m_impl->pinnedNodes.begin(), m_impl->pinnedNodes.end(),
                node) != m_impl->pinnedNodes.end()) continue;
        m_impl->anchoredNodes.push_back({ node, body, anchor.nearMode });
    }

    physics.world->addSoftBody(softBody, collisionLayer, collisionMask);
    m_impl->configurationRevision = GetConfigurationRevision();
    m_impl->renderMeshRevision = mesh->GetConfigurationRevision();
    m_impl->simulationMeshSource = simulationMesh;
    m_impl->simulationMeshPath = simulationMeshPathKey;
    m_impl->simulationMeshRevision = simulationMesh
        ? simulationMesh->GetConfigurationRevision() : 0;
    m_impl->worldScale = scale;
    return true;
}

void Engine::Components::Cloth::DestroySoftBody(bool restoreMesh)
{
    if (!m_impl) return;
    if (m_impl->softBody && m_impl->scene)
    {
        Engine::Physics::StateFor(m_impl->scene).world->removeSoftBody(m_impl->softBody.get());
    }
    m_impl->softBody.reset();
    if (restoreMesh && m_impl->mesh && !m_impl->originalVertices.empty())
        m_impl->mesh->SetDeformedVertices(m_impl->originalVertices);
    m_impl->scene = nullptr;
    m_impl->mesh = nullptr;
    m_impl->simulationMeshSource = nullptr;
    m_impl->cachedSimulationMesh.reset();
    m_impl->simulationMeshPath.clear();
    m_impl->configurationRevision = 0;
    m_impl->renderMeshRevision = 0;
    m_impl->simulationMeshRevision = 0;
    m_impl->worldScale = {};
    m_impl->originalVertices.clear();
    m_impl->nodeLocalPositions.clear();
    m_impl->renderToNode.clear();
    m_impl->renderNodeOffsets.clear();
    m_impl->pinnedNodes.clear();
    m_impl->anchoredNodes.clear();
    m_impl->morphWorldNormal = { 0.f, 1.f, 0.f };
    m_impl->morphLocalCenter = { 0.f, 0.f, 0.f };
    m_impl->morphAmount = 0.f;
}

void Engine::Components::Cloth::UpdatePinnedNodes()
{
    if (!m_impl || !m_impl->softBody || !Owner) return;
    const glm::mat4 world = Owner->transform.GetWorldMatrix();
    for (int index : m_impl->pinnedNodes)
    {
        const glm::vec3 position(world * glm::vec4(m_impl->nodeLocalPositions[index], 1.f));
        auto& node = m_impl->softBody->m_nodes[index];
        node.m_x = Engine::Physics::ToBullet(position);
        node.m_q = node.m_x;
        node.m_v.setZero();
    }
}

void Engine::Components::Cloth::BindAnchors()
{
    if (!m_impl || !m_impl->softBody) return;
    // Bullet stores raw rigid-body pointers in its anchors. Rebuild before a
    // replaced or removed body can leave an obsolete pointer in the solver.
    for (const auto& anchor : m_impl->anchoredNodes)
    {
        auto* rigid = anchor.body ? anchor.body->GetComponent<RigidBody>() : nullptr;
        if (anchor.nativeBody && (!rigid ||
            anchor.nativeBody != rigid->GetNativeCollisionObjectForPhysics() ||
            anchor.bodyGeneration != rigid->GetPhysicsBodyGeneration()))
        {
            DestroySoftBody(true);
            if (!EnsureSoftBody()) return;
            break;
        }
    }
    for (auto& anchor : m_impl->anchoredNodes)
    {
        if (anchor.nativeBody) continue;
        auto* rigid = anchor.body ? anchor.body->GetComponent<RigidBody>() : nullptr;
        if (!rigid) continue;
        auto* native = static_cast<btRigidBody*>(
            rigid->GetNativeCollisionObjectForPhysics());
        if (!native) continue;
        // Near mode avoids contact fighting the constraint around the anchor.
        m_impl->softBody->appendAnchor(anchor.index, native, anchor.nearMode);
        anchor.nativeBody = native;
        anchor.bodyGeneration = rigid->GetPhysicsBodyGeneration();
    }
}

void Engine::Components::Cloth::ApplyForces()
{
    if (!m_impl || !m_impl->softBody) return;
    btSoftBody* softBody = m_impl->softBody.get();
    if (windStrength > 0.f && glm::length(windVelocity) > 0.0001f)
        softBody->addForce(Engine::Physics::ToBullet(windVelocity * windStrength));
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

void Engine::Components::Cloth::SyncMeshFromSoftBody()
{
    if (!m_impl || !m_impl->softBody || !m_impl->mesh || !Owner) return;
    std::vector<Engine::Model::AnimationVertex> deformed = m_impl->originalVertices;
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
    // Uploading a simulated frame advances Mesh's revision. It must not make
    // the next physics step treat this transient deformation as new authoring.
    m_impl->renderMeshRevision = m_impl->mesh->GetConfigurationRevision();
    if (m_impl->simulationMeshSource == m_impl->mesh)
        m_impl->simulationMeshRevision = m_impl->renderMeshRevision;
}

void Engine::Components::Cloth::NotifyRigidBodyCollision(
    const glm::vec3& worldNormal, const glm::vec3&, float impulse)
{
    if (!collisionMorph || !m_impl || !m_impl->mesh || impulse <= 0.f)
        return;
    const float bodyMass = Owner
        ? std::max(0.001f, Owner->GetComponent<RigidBody>()
            ? Owner->GetComponent<RigidBody>()->mass : mass)
        : std::max(0.001f, mass);
    const float amount = impulse * std::max(0.f, collisionMorphStrength) /
        bodyMass;
    if (amount <= 0.f)
        return;

    // A rigid body can spread its load across several manifold points.
    // Accumulate those contacts so every render mesh receives the complete
    // compression signal. Opposing normals describe the same squash axis, so
    // align their signs before blending them together.
    if (glm::length(worldNormal) > 0.00001f)
    {
        glm::vec3 normal = glm::normalize(worldNormal);
        if (glm::dot(normal, m_impl->morphWorldNormal) < 0.f)
            normal = -normal;
        const glm::vec3 weightedAxis =
            m_impl->morphWorldNormal * m_impl->morphAmount + normal * amount;
        if (glm::length(weightedAxis) > 0.00001f)
            m_impl->morphWorldNormal = glm::normalize(weightedAxis);
    }
    m_impl->morphAmount = std::min(
        std::max(0.f, collisionMorphMaximum), m_impl->morphAmount + amount);
}

void Engine::Components::Cloth::UpdateCollisionMorph(float deltaTime)
{
    if (!collisionMorph || !m_impl || !m_impl->mesh || !Owner ||
        m_impl->originalVertices.empty())
        return;
    const float decay = std::exp(-std::max(0.f, collisionMorphRecovery) *
        std::max(0.f, deltaTime));
    m_impl->morphAmount *= decay;
    if (m_impl->morphAmount < 0.0001f)
    {
        m_impl->morphAmount = 0.f;
        m_impl->mesh->SetDeformedVertices(m_impl->originalVertices);
        return;
    }

    const glm::mat3 inverseWorld = glm::inverse(glm::mat3(
        Owner->transform.GetWorldMatrix()));
    glm::vec3 localNormal = inverseWorld * m_impl->morphWorldNormal;
    localNormal = glm::length(localNormal) > 0.00001f
        ? glm::normalize(localNormal) : glm::vec3(0.f, 1.f, 0.f);
    const float squeeze = std::clamp(m_impl->morphAmount, 0.f,
        std::max(0.f, collisionMorphMaximum));
    const float expand = 1.f + squeeze *
        std::clamp(collisionMorphExpansion, 0.f, 1.f);
    std::vector<Engine::Model::AnimationVertex> deformed = m_impl->originalVertices;
    for (Engine::Model::AnimationVertex& vertex : deformed)
    {
        const glm::vec3 original(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const glm::vec3 relative = original - m_impl->morphLocalCenter;
        const glm::vec3 parallel = localNormal * glm::dot(relative, localNormal);
        const glm::vec3 position = m_impl->morphLocalCenter +
            parallel * (1.f - squeeze) + (relative - parallel) * expand;
        vertex.pos[0] = position.x;
        vertex.pos[1] = position.y;
        vertex.pos[2] = position.z;
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

void Engine::Components::Cloth::ResetSimulation()
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().ResetCloth(*this);
    else NativeResetSimulation();
}

void Engine::Components::Cloth::NativeResetSimulation()
{
    DestroySoftBody(true);
    if (collisionMorph) EnsureCollisionMorph();
    else EnsureSoftBody();
}
void Engine::Components::Cloth::Start() {}
void Engine::Components::Cloth::Update() {}
void Engine::Components::Cloth::Enabled() {}
void Engine::Components::Cloth::Disabled()
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->GetPhysics().DestroyCloth(*this);
    else DestroySoftBody(true);
}
void Engine::Components::Cloth::OnDestroy() { Disabled(); }

}
