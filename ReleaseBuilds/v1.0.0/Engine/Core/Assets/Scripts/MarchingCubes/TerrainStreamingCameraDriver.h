#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include <glm/glm.hpp>

// Moves a viewer repeatedly across chunk boundaries so terrain streaming can
// be profiled without manual input. The endpoints are authored world-space
// positions and movement reverses after an optional pause at either end.
class TerrainStreamingCameraDriver final : public Engine::Core::Script
{
public:
    TerrainStreamingCameraDriver();

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe")
    glm::vec3 startPosition { -48.f, 14.f, 8.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe")
    glm::vec3 endPosition { 64.f, 14.f, 8.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe", ClampMin = "0.01")
    float movementSpeed = 18.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe", ClampMin = "0")
    float pauseAtEndpoints = 0.75f;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe")
    bool moveOnStart = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe")
    bool moveInfinitely = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe")
    bool lookInMovementDirection = true;

    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe", ClampMin = "0", ClampMax = "1")
    float downwardLook = 0.35f;

    // Script updates currently have no engine delta-time argument. Keeping the
    // probe on the same fixed step as headless scene simulation makes server
    // runs deterministic and ensures they actually cross chunk boundaries.
    PROPERTY(Inspector, EditAnywhere, Category = "Terrain Streaming Probe", ClampMin = "0.0001")
    float secondsPerUpdate = 1.f / 60.f;

    void Start() override;
    void Update() override;

private:
    float m_pauseRemaining = 0.f;
    bool m_towardEnd = true;
};
