#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Collider.h"
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace
{
bool Finite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

Engine::Components::SpatialManipulator* AddHorizontalPortal(
    Engine::Scene::Scene& scene, const char* name, float height,
    float rotationX)
{
    using namespace Engine::Components;
    Engine::Core::Object* object = scene.AddObject(name);
    object->transform.position = glm::vec3(0.f, height, 0.f);
    object->transform.rotation = glm::vec3(rotationX, 0.f, 0.f);

    auto* body = object->AddComponent<RigidBody>();
    body->bodyType = "Static";
    body->isTrigger = true;
    body->useGravity = false;
    body->collisionLayer = 2;

    auto* collider = object->AddComponent<PrimitiveObjectCollider>();
    collider->shape = "Box";
    collider->size = glm::vec3(6.f, 6.f, 1.f);

    auto* portal = object->AddComponent<SpatialManipulator>();
    portal->connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    portal->portalPointCount = 4;
    portal->portalShapePoint0 = glm::vec3(-2.5f, -2.5f, 0.f);
    portal->portalShapePoint1 = glm::vec3( 2.5f, -2.5f, 0.f);
    portal->portalShapePoint2 = glm::vec3( 2.5f,  2.5f, 0.f);
    portal->portalShapePoint3 = glm::vec3(-2.5f,  2.5f, 0.f);
    portal->portalEdgeHalfWidth = 0.04f;
    portal->portalEdgeHalfDepth = 0.01f;
    portal->traversalBlendDistance = 0.75f;
    return portal;
}
}

int main(int argc, char** argv)
{
    using namespace Engine::Components;
    Engine::Scene::Scene scene;
    constexpr float loopHeight = 12.f;
    auto* floorPortal = AddHorizontalPortal(scene, "Floor Portal", 0.f,
        -glm::half_pi<float>());
    auto* ceilingPortal = AddHorizontalPortal(scene, "Ceiling Portal",
        loopHeight, glm::half_pi<float>());
    floorPortal->ConnectToTarget(ceilingPortal);
    const glm::vec3 floorNormal = glm::normalize(glm::vec3(
        floorPortal->GetPortalWorldFrame()[2]));
    const glm::vec3 ceilingNormal = glm::normalize(glm::vec3(
        ceilingPortal->GetPortalWorldFrame()[2]));
    const glm::vec3 mappedDown = glm::mat3(
        floorPortal->GetPortalWorldTransformTo(*ceilingPortal)) *
        glm::vec3(0.f, -1.f, 0.f);
    if (glm::dot(floorNormal, glm::vec3(0.f, 1.f, 0.f)) < 0.999f ||
        glm::dot(ceilingNormal, glm::vec3(0.f, -1.f, 0.f)) < 0.999f ||
        glm::dot(glm::normalize(mappedDown), glm::vec3(0.f, -1.f, 0.f)) < 0.999f)
    {
        std::fprintf(stderr, "Vertical portal pair is not facing or mapping downward correctly\n");
        return 1;
    }

    Engine::Core::Object* cube = scene.AddObject("Gravity Loop Cube");
    cube->transform.position = glm::vec3(0.f, 8.f, 0.f);
    cube->transform.scale = glm::vec3(0.5f);
    auto* body = cube->AddComponent<RigidBody>();
    body->bodyType = "Dynamic";
    body->mass = 1.f;
    body->useGravity = true;
    body->gravityScale = 1.f;
    body->linearDamping = 0.f;
    body->angularDamping = 0.f;
    body->continuousCollision = true;
    body->collisionMask = ~2;
    body->freezeRotationX = true;
    body->freezeRotationY = true;
    body->freezeRotationZ = true;
    auto* collider = cube->AddComponent<PrimitiveObjectCollider>();
    collider->shape = "Sphere";
    collider->radius = 0.25f;

    scene.Start();
    const float updateHz = argc > 1
        ? std::clamp(static_cast<float>(std::atof(argv[1])), 30.f, 1000.f)
        : 120.f;
    const float timeStep = 1.f / updateHz;
    const int maximumSteps = static_cast<int>(updateHz * 180.f);
    glm::vec3 previousPosition = cube->transform.GetWorldPosition();
    float peakStableSpeed = 0.f;
    int traversalCount = 0;
    int failureStep = -1;
    const char* failureReason = "time limit";

    for (int step = 0; step < maximumSteps; ++step)
    {
        scene.Update(timeStep);
        const glm::vec3 position = cube->transform.GetWorldPosition();
        const glm::vec3 velocity = body->GetLinearVelocity();
        const float speed = glm::length(velocity);
        if (!Finite(position) || !Finite(velocity) || !std::isfinite(speed))
        {
            failureStep = step;
            failureReason = "non-finite physics state";
            break;
        }
        if (position.y - previousPosition.y > loopHeight * 0.5f)
            ++traversalCount;
        // Once below the lower trigger volume, the swept portal crossing was
        // missed. This is the first practical speed limit for this fixture.
        if (position.y < -1.f)
        {
            failureStep = step;
            failureReason = "portal crossing missed";
            break;
        }
        // The paired rotations must preserve a downward gravity trajectory.
        if (velocity.y > 0.5f || std::abs(velocity.x) > 0.5f ||
            std::abs(velocity.z) > 0.5f)
        {
            failureStep = step;
            failureReason = "velocity direction corrupted";
            break;
        }
        peakStableSpeed = std::max(peakStableSpeed, speed);
        previousPosition = position;
    }

    std::printf("vertical_portal_loop update_hz=%.1f traversals=%d "
        "peak_stable_speed=%.3f_mps "
        "failure_step=%d simulated_seconds=%.3f reason=%s\n",
        updateHz, traversalCount, peakStableSpeed, failureStep,
        (failureStep < 0 ? maximumSteps : failureStep) * timeStep,
        failureReason);

    if (traversalCount < 3 || peakStableSpeed < 25.f)
    {
        std::fprintf(stderr, "Vertical portal loop failed before baseline stability\n");
        return 1;
    }
    return 0;
}
