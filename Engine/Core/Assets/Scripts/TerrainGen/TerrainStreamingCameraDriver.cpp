#include "Scripts/TerrainGen/TerrainStreamingCameraDriver.h"
#include "Scripts/TerrainGen/TerrainGen.h"

#include "Core/Object.h"
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
    if (Owner && moveOnStart)
    {
        Owner->transform.position = startPosition;
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
    if (!m_initialTerrainReady)
    {
        Engine::Core::Object* terrainObject = terrainObjectName.empty()
            ? nullptr : Owner->FindObjectInSceneByName(terrainObjectName);
        TerrainGen* terrain = terrainObject
            ? terrainObject->GetComponent<TerrainGen>() : nullptr;
        if (!terrain)
            return;
        const int diameter = std::clamp(terrain->viewRadiusInChunks, 0, 8) * 2 + 1;
        const size_t desiredChunks = static_cast<size_t>(diameter) * diameter;
        if (terrain->GetLoadedChunkCount() < desiredChunks ||
            terrain->GetQueuedChunkCount() != 0u ||
            terrain->GetInFlightChunkCount() != 0u)
            return;
        m_initialTerrainReady = true;
    }
    const float deltaTime = std::clamp(secondsPerUpdate, 0.0001f, 0.25f);
    if (moveInfinitely)
    {
        glm::vec3 direction = endPosition - startPosition;
        direction.y = 0.f;
        const float length = glm::length(direction);
        if (length > 0.0001f)
            Owner->transform.position += direction / length *
                std::max(0.01f, movementSpeed) * deltaTime;
        return;
    }
    if (m_pauseRemaining > 0.f)
    {
        m_pauseRemaining = std::max(0.f, m_pauseRemaining - deltaTime);
        return;
    }

    const glm::vec3 target = m_towardEnd ? endPosition : startPosition;
    const glm::vec3 delta = target - Owner->transform.position;
    const float distance = glm::length(delta);
    const float step = std::max(0.01f, movementSpeed) * deltaTime;
    if (distance <= step || distance <= 0.0001f)
    {
        Owner->transform.position = target;
        m_towardEnd = !m_towardEnd;
        m_pauseRemaining = std::max(0.f, pauseAtEndpoints);
        return;
    }
    Owner->transform.position += delta * (step / distance);
}
