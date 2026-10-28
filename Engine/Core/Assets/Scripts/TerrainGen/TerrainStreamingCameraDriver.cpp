#include "Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Scripts/TerrainGen/TerrainGen.h"

#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

TerrainStreamingCameraDriver::TerrainStreamingCameraDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(TerrainStreamingCameraDriver));
    RegisterField("startPosition", startPosition);
    RegisterField("endPosition", endPosition);
    RegisterField("movementSpeed", movementSpeed);
    RegisterField("pauseAtEndpoints", pauseAtEndpoints);
    RegisterField("moveOnStart", moveOnStart);
    RegisterField("waitForInitialTerrain", waitForInitialTerrain);
    RegisterField("terrainObjectName", terrainObjectName);
    RegisterField("moveInfinitely", moveInfinitely);
    RegisterField("lookInMovementDirection", lookInMovementDirection);
    RegisterField("usePhysicsBody", usePhysicsBody);
    RegisterField("terrainGroundOffset", terrainGroundOffset);
    RegisterField("downwardLook", downwardLook);
    RegisterField("secondsPerUpdate", secondsPerUpdate);
}

namespace
{
struct TerrainStreamingCameraDriverRegistration
{
    TerrainStreamingCameraDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<TerrainStreamingCameraDriver>(
            "TerrainStreamingCameraDriver");
    }
};
TerrainStreamingCameraDriverRegistration g_registration;
}

void TerrainStreamingCameraDriver::Start()
{
    m_pauseRemaining = 0.f;
    m_towardEnd = true;
    m_initialTerrainReady = !waitForInitialTerrain;
    m_groundSnapped = false;
    if (Owner && moveOnStart)
    {
        if (usePhysicsBody)
        {
            if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
                body->SetWorldPosition(startPosition);
            else
                Owner->transform.position = startPosition;
        }
        else Owner->transform.position = startPosition;
        if (lookInMovementDirection)
        {
            glm::vec3 travel = endPosition - startPosition;
            travel.y = 0.f;
            if (glm::dot(travel, travel) > 0.000001f)
            {
                travel = glm::normalize(travel);
                const glm::vec3 forward = glm::normalize(travel +
                    glm::vec3(0.f, -std::clamp(downwardLook, 0.f, 1.f), 0.f));
                const glm::vec3 right = glm::normalize(glm::cross(
                    glm::vec3(0.f, 1.f, 0.f), forward));
                const glm::vec3 up = glm::normalize(glm::cross(forward, right));
                glm::mat3 basis(1.f);
                basis[0] = right;
                basis[1] = up;
                basis[2] = forward;
                Owner->transform.rotation = glm::eulerAngles(
                    glm::normalize(glm::quat_cast(basis)));
            }
        }
    }
}

void TerrainStreamingCameraDriver::Update()
{
    if (!Owner || !moveOnStart)
        return;
    const auto updateLookFromDirection = [&](glm::vec3 direction) {
        if (!lookInMovementDirection) return;
        direction.y = 0.f;
        if (glm::dot(direction, direction) <= 0.000001f) return;
        direction = glm::normalize(direction);
        const glm::vec3 forward = glm::normalize(direction +
            glm::vec3(0.f, -std::clamp(downwardLook, 0.f, 1.f), 0.f));
        const glm::vec3 right = glm::normalize(glm::cross(
            glm::vec3(0.f, 1.f, 0.f), forward));
        const glm::vec3 up = glm::normalize(glm::cross(forward, right));
        glm::mat3 basis(1.f);
        basis[0] = right;
        basis[1] = up;
        basis[2] = forward;
        Owner->transform.rotation = glm::eulerAngles(
            glm::normalize(glm::quat_cast(basis)));
    };
    if (!m_initialTerrainReady)
    {
        const auto holdAtStart = [&]() {
            if (!usePhysicsBody) return;
            if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
            {
                body->SetWorldPosition(startPosition);
                body->SetLinearVelocity(glm::vec3(0.f));
            }
        };
        Engine::Core::Object* terrainObject = terrainObjectName.empty()
            ? nullptr : Owner->FindObjectInSceneByName(terrainObjectName);
        if (!terrainObject)
        {
            holdAtStart();
            return;
        }
        if (TerrainGen* terrain = terrainObject->GetComponent<TerrainGen>())
        {
            const int diameter =
                std::clamp(terrain->viewRadiusInChunks, 0, 8) * 2 + 1;
            const size_t desiredChunks =
                static_cast<size_t>(diameter) * diameter;
            if (terrain->GetLoadedChunkCount() < desiredChunks ||
                terrain->GetQueuedChunkCount() != 0u ||
                terrain->GetInFlightChunkCount() != 0u)
            {
                holdAtStart();
                return;
            }
        }
        else if (auto* manager = terrainObject->GetComponent<
            Engine::Components::TerrainManager>())
        {
            if (manager->GetLoadedCellCount() == 0u)
            {
                holdAtStart();
                return;
            }
            if (usePhysicsBody && !m_groundSnapped)
            {
                double groundY = 0.0;
                const glm::dvec3 world = Owner->transform.GetWorldPositionD();
                if (manager->TryGetHeightAtWorldPosition(world, groundY))
                {
                    glm::dvec3 snapped = world;
                    snapped.y = groundY + std::max(0.f, terrainGroundOffset);
                    if (auto* body = Owner->GetComponent<
                        Engine::Components::RigidBody>())
                    {
                        body->SetWorldPosition(glm::vec3(snapped));
                        body->SetLinearVelocity(glm::vec3(0.f));
                    }
                    else
                        Owner->transform.position = snapped;
                    m_groundSnapped = true;
                }
            }
        }
        else
        {
            holdAtStart();
            return;
        }
        m_initialTerrainReady = true;
    }
    const float sceneDeltaTime = Owner->GetScene()
        ? Owner->GetScene()->GetDeltaTime() : 0.f;
    const float deltaTime = std::clamp(sceneDeltaTime > 0.f
        ? sceneDeltaTime : secondsPerUpdate, 0.0001f, 0.25f);
    if (moveInfinitely)
    {
        glm::vec3 direction = endPosition - startPosition;
        direction.y = 0.f;
        const float length = glm::length(direction);
        if (length > 0.0001f)
        {
            const glm::vec3 travelDirection = direction / length;
            updateLookFromDirection(travelDirection);
            const float speed = std::max(0.01f, movementSpeed);
            if (usePhysicsBody)
            {
                if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
                {
                    glm::vec3 velocity = body->GetLinearVelocity();
                    velocity.x = travelDirection.x * speed;
                    velocity.z = travelDirection.z * speed;
                    body->SetLinearVelocity(velocity);
                }
                else
                {
                    Owner->transform.position += travelDirection * speed * deltaTime;
                }
            }
            else
            {
                Owner->transform.position += travelDirection * speed * deltaTime;
            }
        }
        return;
    }
    if (m_pauseRemaining > 0.f)
    {
        if (usePhysicsBody)
            if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
            {
                glm::vec3 velocity = body->GetLinearVelocity();
                velocity.x = 0.f;
                velocity.z = 0.f;
                body->SetLinearVelocity(velocity);
            }
        m_pauseRemaining = std::max(0.f, m_pauseRemaining - deltaTime);
        return;
    }

    const glm::dvec3 target = glm::dvec3(m_towardEnd ? endPosition : startPosition);
    const glm::dvec3 delta = target - Owner->transform.position;
    auto* physicsBody = usePhysicsBody
        ? Owner->GetComponent<Engine::Components::RigidBody>() : nullptr;
    // A physics body follows the ground. Its height must not prevent it from
    // reaching a horizontal waypoint or make it drive through the map edge.
    const glm::dvec3 travelDelta = physicsBody
        ? glm::dvec3(delta.x, 0.0, delta.z) : delta;
    const double distance = glm::length(travelDelta);
    if (distance > 0.0001)
        updateLookFromDirection(glm::vec3(travelDelta / distance));
    const double step = static_cast<double>(std::max(0.01f, movementSpeed)) * deltaTime;
    if (distance <= step || distance <= 0.0001f)
    {
        if (usePhysicsBody)
        {
            if (physicsBody)
            {
                glm::dvec3 reached = Owner->transform.GetWorldPositionD();
                reached.x = target.x;
                reached.z = target.z;
                physicsBody->SetWorldPosition(glm::vec3(reached));
                glm::vec3 velocity = physicsBody->GetLinearVelocity();
                velocity.x = 0.f;
                velocity.z = 0.f;
                physicsBody->SetLinearVelocity(velocity);
            }
            else
                Owner->transform.position = target;
        }
        else
            Owner->transform.position = target;
        m_towardEnd = !m_towardEnd;
        m_pauseRemaining = std::max(0.f, pauseAtEndpoints);
        return;
    }
    if (usePhysicsBody)
    {
        if (physicsBody)
        {
            const glm::dvec3 direction = travelDelta / distance;
            updateLookFromDirection(glm::vec3(direction));
            glm::vec3 velocity = physicsBody->GetLinearVelocity();
            const float speed = std::max(0.01f, movementSpeed);
            velocity.x = static_cast<float>(direction.x) * speed;
            velocity.z = static_cast<float>(direction.z) * speed;
            physicsBody->SetLinearVelocity(velocity);
        }
        else
            Owner->transform.position += delta * (step / distance);
    }
    else
        Owner->transform.position += delta * (step / distance);
}
