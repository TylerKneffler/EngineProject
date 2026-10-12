#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Serialization/SceneSerializer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>

using Engine::Components::PrimitiveObjectCollider;
using Engine::Components::RigidBody;
using Engine::Components::SpatialManipulator;
using Engine::Core::Object;
using Engine::Physics::Physics;
using Engine::Scene::Scene;

namespace
{
RigidBody* AddBox(Scene& scene, const char* name, const glm::vec3& position)
{
    Object* object = scene.AddObject(name);
    object->transform.position = position;
    auto* body = object->AddComponent<RigidBody>();
    body->bodyType = "Static";
    body->useGravity = false;
    auto* collider = object->AddComponent<PrimitiveObjectCollider>();
    collider->shape = "Box";
    collider->size = glm::vec3(1.f);
    return body;
}

void ConfigurePortal(SpatialManipulator& portal)
{
    portal.connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    portal.portalNormal = glm::vec3(0.f, 0.f, 1.f);
    portal.portalPointCount = 4;
    portal.portalShapePoint0 = {-1.f, -1.f, 0.f};
    portal.portalShapePoint1 = { 1.f, -1.f, 0.f};
    portal.portalShapePoint2 = { 1.f,  1.f, 0.f};
    portal.portalShapePoint3 = {-1.f,  1.f, 0.f};
}
}

int main()
{
    // Derived fragments never duplicate authored behavior or serialization.
    {
        Scene scene;
        Object* owner = scene.AddObject("authored owner");
        Object* fragment = scene.AddObject("runtime portal fragment");
        fragment->runtimeOnly = true;
        fragment->Parent = owner;
        owner->Children.push_back(fragment);
        const std::string saved =
            Engine::Serialization::SceneSerializer::SaveToString(scene);
        assert(saved.find("authored owner") != std::string::npos);
        assert(saved.find("runtime portal fragment") == std::string::npos);
    }

    // Indexed topology is expanded only for the cut snapshot; morph and skin
    // deformation are evaluated before expansion.
    {
        Engine::Components::Mesh mesh;
        std::vector<Engine::Components::Mesh::Vertex> vertices(4);
        vertices[0].pos[0] = -1.f; vertices[0].pos[1] = -1.f;
        vertices[1].pos[0] =  1.f; vertices[1].pos[1] = -1.f;
        vertices[2].pos[0] =  1.f; vertices[2].pos[1] =  1.f;
        vertices[3].pos[0] = -1.f; vertices[3].pos[1] =  1.f;
        for (auto& vertex : vertices)
        {
            vertex.normal[2] = 1.f;
            vertex.joints0[0] = 0.f;
            vertex.weights0[0] = 1.f;
        }
        assert(mesh.SetIndexedGeometry(std::move(vertices),
            {0u, 1u, 2u, 0u, 2u, 3u}));
        Engine::Model::MorphTarget morph;
        morph.positions.assign(4u, glm::vec3(0.f, 0.f, 1.f));
        mesh.SetMorphData(0u, {morph}, {0.5f});
        std::vector<glm::mat4> palette { glm::translate(
            glm::mat4(1.f), glm::vec3(0.f, 0.f, 2.f)) };
        const auto stream = mesh.BuildPortalCutTriangleStream(&palette);
        assert(stream.size() == 6u);
        assert(std::abs(stream[0].pos[2] - 2.5f) < 0.0001f);
        assert(std::abs(stream[3].pos[0] - stream[0].pos[0]) < 0.0001f);
    }

    // Ordinary Bullet rigid body.
    {
        Scene scene;
        RigidBody* body = AddBox(scene, "ordinary", {0.f, 0.f, 5.f});
        scene.GetPhysics().Step(0.f);
        const auto hits = scene.GetPhysics().RaycastAll(
            {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 10.f);
        assert(!hits.empty());
        assert(hits.front().kind == Physics::RaycastHitKind::RigidBody);
        assert(hits.front().rigidBody == body);
        assert(std::abs(hits.front().distance - 4.5f) < 0.02f);
    }

    // Remote collision half produced while a body is split by a portal.
    {
        Scene scene;
        RigidBody* owner = AddBox(scene, "split owner", {20.f, 0.f, 0.f});
        scene.GetPhysics().Step(0.f);
        int splitKey = 0;
        scene.GetPhysics().SetPortalMeshCollider(&splitKey, *owner, {
            {-1.f, -1.f, 3.f}, {1.f, -1.f, 3.f}, {0.f, 1.f, 3.f}
        });
        const auto hits = scene.GetPhysics().RaycastAll(
            {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 10.f);
        assert(!hits.empty());
        assert(hits.front().kind == Physics::RaycastHitKind::PortalSplitPiece);
        assert(hits.front().rigidBody == owner);
        assert(hits.front().instanceKey == &splitKey);
    }

    // Solid generated rim; the centre remains an open aperture.
    {
        Scene scene;
        int rimKey = 0;
        scene.GetPhysics().SetPortalApertureCollider(&rimKey, {
            {-1.f, -1.f, 3.f}, {1.f, -1.f, 3.f},
            {1.f, 1.f, 3.f}, {-1.f, 1.f, 3.f}
        }, {0.f, 0.f, 1.f}, 0.1f, 0.1f);
        const auto centreHits = scene.GetPhysics().RaycastAll(
            {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 10.f);
        assert(centreHits.empty());
        const auto rimHits = scene.GetPhysics().RaycastAll(
            {1.1f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 10.f);
        assert(!rimHits.empty());
        assert(rimHits.front().kind == Physics::RaycastHitKind::PortalApertureRim);
        assert(rimHits.front().instanceKey == &rimKey);
    }

    // Recursive scene query: enter one portal and hit an ordinary target in
    // its connected chart, reporting total path distance and hop count.
    {
        Scene scene;
        Object* sourceObject = scene.AddObject("source portal");
        sourceObject->transform.position = {0.f, 0.f, 2.f};
        auto* source = sourceObject->AddComponent<SpatialManipulator>();
        ConfigurePortal(*source);
        Object* targetObject = scene.AddObject("target portal");
        targetObject->transform.position = {0.f, 0.f, 10.f};
        auto* target = targetObject->AddComponent<SpatialManipulator>();
        ConfigurePortal(*target);
        source->ConnectToTarget(target);
        RigidBody* targetBody = AddBox(scene, "remote target", {0.f, 0.f, 7.f});
        scene.GetPhysics().Step(0.f);

        Scene::PortalRaycastHit hit;
        assert(scene.RaycastPortals({{0.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
            20.f, hit, 4u));
        assert(hit.physicsHit.rigidBody == targetBody);
        assert(hit.portalHops == 1u);
        assert(hit.distance > 4.f && hit.distance < 6.f);
    }
    return 0;
}
