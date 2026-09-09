#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Scripts/PortalTraversalRepeater.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
bool Near(float actual, float expected, float tolerance = 0.03f)
{
    return std::abs(actual - expected) <= tolerance;
}

Engine::Components::SpatialManipulator* AddPortal(
    Engine::Scene::Scene& scene, const char* name, const glm::vec3& position,
    float halfExtent)
{
    using namespace Engine::Components;
    Engine::Core::Object* object = scene.AddObject(name);
    object->transform.position = position;

    auto* body = object->AddComponent<RigidBody>();
    body->bodyType = "Static";
    body->isTrigger = true;
    body->useGravity = false;
    body->collisionLayer = 2;

    auto* collider = object->AddComponent<PrimitiveObjectCollider>();
    collider->shape = "Box";
    collider->size = glm::vec3(halfExtent * 2.f + 1.f,
        halfExtent * 2.f + 1.f, 2.f);

    auto* portal = object->AddComponent<SpatialManipulator>();
    portal->connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    portal->portalPointCount = 4;
    portal->portalShapePoint0 = glm::vec3(-halfExtent, -halfExtent, 0.f);
    portal->portalShapePoint1 = glm::vec3( halfExtent, -halfExtent, 0.f);
    portal->portalShapePoint2 = glm::vec3( halfExtent,  halfExtent, 0.f);
    portal->portalShapePoint3 = glm::vec3(-halfExtent,  halfExtent, 0.f);
    portal->portalEdgeHalfWidth = 0.03f;
    portal->portalEdgeHalfDepth = 0.05f;
    portal->deformMeshOnTraversal = false;
    return portal;
}

Engine::Components::RigidBody* AddTraverser(Engine::Scene::Scene& scene,
    const char* name, const glm::vec3& position, float objectScale,
    const glm::vec3& velocity)
{
    using namespace Engine::Components;
    Engine::Core::Object* object = scene.AddObject(name);
    object->transform.position = position;
    object->transform.scale = glm::vec3(objectScale);

    auto* body = object->AddComponent<RigidBody>();
    body->bodyType = "Dynamic";
    body->useGravity = false;
    body->linearDamping = 0.f;
    body->angularDamping = 0.f;
    body->continuousCollision = true;
    body->collisionMask = ~2;
    body->initialLinearVelocity = velocity;

    auto* collider = object->AddComponent<PrimitiveObjectCollider>();
    collider->shape = "Sphere";
    collider->radius = 0.25f;
    return body;
}
}

int main()
{
    using namespace Engine::Components;
    Mesh diagnosticCube;
    diagnosticCube.LoadFromFile("Assets/Mesh/diagnostic_cube.obj");
    const auto& diagnosticVertices = diagnosticCube.GetVertices();
    if (diagnosticVertices.size() != 36u)
    {
        std::fprintf(stderr, "Diagnostic cube did not load 12 triangles\n");
        return 1;
    }
    // Each face occupies six consecutive vertices and must retain a distinct
    // color through the OBJ loader. Opposite sides are intentionally obvious.
    for (size_t firstFace = 0; firstFace < 6u; ++firstFace)
    {
        for (size_t secondFace = firstFace + 1u; secondFace < 6u; ++secondFace)
        {
            const auto& first = diagnosticVertices[firstFace * 6u];
            const auto& second = diagnosticVertices[secondFace * 6u];
            const float colorDistance = std::abs(first.color[0] - second.color[0]) +
                std::abs(first.color[1] - second.color[1]) +
                std::abs(first.color[2] - second.color[2]);
            if (colorDistance < 0.2f)
            {
                std::fprintf(stderr, "Diagnostic cube face colors are not distinct\n");
                return 1;
            }
        }
    }

    Engine::Scene::Scene scene;
    auto* smallPortal = AddPortal(scene, "Small Portal", glm::vec3(0.f), 1.f);
    auto* largePortal = AddPortal(scene, "Large Portal",
        glm::vec3(10.f, 0.f, 0.f), 2.f);
    smallPortal->ConnectToTarget(largePortal);

    const float outwardRatio = smallPortal->GetPortalScaleRatioTo(*largePortal);
    const float returnRatio = largePortal->GetPortalScaleRatioTo(*smallPortal);
    if (!Near(outwardRatio, 2.f, 0.001f) ||
        !Near(returnRatio, 0.5f, 0.001f) ||
        !Near(outwardRatio * returnRatio, 1.f, 0.001f))
    {
        std::fprintf(stderr, "Portal aperture ratios are not reciprocal\n");
        return 1;
    }

    RigidBody* growingBody = AddTraverser(scene, "Growing Cube",
        glm::vec3(0.f, 0.f, -2.f), 0.4f, glm::vec3(0.f, 0.f, 4.f));
    RigidBody* shrinkingBody = AddTraverser(scene, "Shrinking Cube",
        glm::vec3(10.f, 0.f, 2.f), 0.8f, glm::vec3(0.f, 0.f, -4.f));
    shrinkingBody->Owner->enabled = false;

    scene.Start();
    bool grew = false;
    bool shrank = false;
    for (int frame = 0; frame < 240; ++frame)
    {
        if (frame == 90)
            shrinkingBody->Owner->enabled = true;
        scene.Update(1.f / 120.f);

        if (!grew && growingBody->Owner->transform.GetWorldPosition().x > 5.f)
        {
            grew = Near(growingBody->Owner->transform.scale.x, 0.8f) &&
                Near(glm::length(growingBody->GetLinearVelocity()), 8.f, 0.08f);
        }
        if (!shrank && shrinkingBody->Owner->transform.GetWorldPosition().x < 5.f)
        {
            shrank = Near(shrinkingBody->Owner->transform.scale.x, 0.4f) &&
                Near(glm::length(shrinkingBody->GetLinearVelocity()), 2.f, 0.08f);
        }
    }

    if (!grew || !shrank)
    {
        std::fprintf(stderr,
            "Mismatched portal traversal failed: grew=%d shrank=%d "
            "growingScale=%.3f shrinkingScale=%.3f\n",
            grew, shrank, growingBody->Owner->transform.scale.x,
            shrinkingBody->Owner->transform.scale.x);
        return 1;
    }

    // End-to-end non-similar traversal: raise one target corner and verify
    // every rendered cube vertex receives the same piecewise map as a direct
    // portal query. This catches regressions where only the body origin moves.
    Engine::Scene::Scene morphScene;
    auto* morphSource = AddPortal(morphScene, "Morph Source",
        glm::vec3(0.f), 1.f);
    auto* morphTarget = AddPortal(morphScene, "Morph Target",
        glm::vec3(10.f, 0.f, 0.f), 2.f);
    morphTarget->portalShapePoint2.y = 3.f;
    morphSource->ConnectToTarget(morphTarget);
    if (!morphSource->UsesPiecewisePortalWarpTo(*morphTarget))
    {
        std::fprintf(stderr, "Morphed portal did not select piecewise mapping\n");
        return 1;
    }

    RigidBody* morphBody = AddTraverser(morphScene, "Morphed Cube",
        glm::vec3(0.f, 0.f, -2.f), 0.4f, glm::vec3(0.f, 0.f, 4.f));
    Mesh* morphMesh = morphBody->Owner->AddComponent<Mesh>();
    morphMesh->LoadFromFile("Assets/Mesh/diagnostic_cube.obj");
    const std::vector<Engine::Model::Vertex> originalMorphVertices =
        morphMesh->GetVertices();
    const glm::mat4 originalMorphWorld =
        morphBody->Owner->transform.GetWorldMatrix();

    morphScene.Start();
    bool morphTeleported = false;
    float maximumMappedVertexError = 0.f;
    for (int frame = 0; frame < 180 && !morphTeleported; ++frame)
    {
        morphScene.Update(1.f / 120.f);
        if (morphBody->Owner->transform.GetWorldPosition().x <= 5.f)
            continue;
        morphTeleported = true;
        const glm::mat4 mappedWorld = morphBody->Owner->transform.GetWorldMatrix();
        const auto& mappedVertices = morphMesh->GetVertices();
        if (mappedVertices.size() != originalMorphVertices.size())
        {
            std::fprintf(stderr, "Morphed cube vertex count changed\n");
            return 1;
        }
        for (size_t index = 0; index < mappedVertices.size(); ++index)
        {
            const auto& original = originalMorphVertices[index];
            const glm::vec3 originalWorld(originalMorphWorld * glm::vec4(
                original.pos[0], original.pos[1], original.pos[2], 1.f));
            const glm::vec3 expected = morphSource->MapWorldPointThroughPortalShape(
                originalWorld, *morphTarget);
            const auto& mapped = mappedVertices[index];
            const glm::vec3 actual(mappedWorld * glm::vec4(
                mapped.pos[0], mapped.pos[1], mapped.pos[2], 1.f));
            // X/Y are independent of crossing depth. The body advances in Z
            // before handoff, so compare the transverse shape deformation.
            maximumMappedVertexError = std::max(maximumMappedVertexError,
                glm::length(glm::vec2(actual - expected)));
        }
    }
    if (!morphTeleported || maximumMappedVertexError > 0.02f ||
        !morphBody->Owner->GetComponent<MeshObjectCollider>())
    {
        std::fprintf(stderr,
            "Piecewise cube traversal failed: teleported=%d vertexError=%.4f\n",
            morphTeleported, maximumMappedVertexError);
        return 1;
    }

    // A one-shot horizontal demo can hide defects in the reciprocal mapping.
    // Exercise the scene helper through several complete out-and-back trips.
    Engine::Scene::Scene repeatScene;
    auto* repeatSource = AddPortal(repeatScene, "Repeat Source",
        glm::vec3(0.f), 1.f);
    auto* repeatTarget = AddPortal(repeatScene, "Repeat Target",
        glm::vec3(10.f, 0.f, 0.f), 1.f);
    repeatSource->ConnectToTarget(repeatTarget);
    RigidBody* repeatBody = AddTraverser(repeatScene, "Repeating Cube",
        glm::vec3(0.f, 0.f, -2.f), 0.4f, glm::vec3(0.f, 0.f, 4.f));
    auto* repeater = repeatBody->Owner->AddComponent<PortalTraversalRepeater>();
    repeater->sourcePortalObjectName = "Repeat Source";
    repeater->targetPortalObjectName = "Repeat Target";
    repeater->reverseClearance = 1.f;
    repeater->teleportDetectionDistance = 4.f;

    repeatScene.Start();
    bool wasAtTarget = false;
    int reciprocalHandoffs = 0;
    for (int frame = 0; frame < 900; ++frame)
    {
        repeatScene.Update(1.f / 120.f);
        const bool isAtTarget =
            repeatBody->Owner->transform.GetWorldPosition().x > 5.f;
        if (isAtTarget != wasAtTarget)
        {
            ++reciprocalHandoffs;
            wasAtTarget = isAtTarget;
        }
    }
    if (reciprocalHandoffs < 4)
    {
        std::fprintf(stderr,
            "Portal traversal repeater stopped after %d reciprocal handoffs\n",
            reciprocalHandoffs);
        return 1;
    }

    std::printf("portal_scale_traversal forward_ratio=%.2f reverse_ratio=%.2f "
        "piecewise_vertex_error=%.5f reciprocal_handoffs=%d\n",
        outwardRatio, returnRatio, maximumMappedVertexError,
        reciprocalHandoffs);
    return 0;
}
