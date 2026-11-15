#pragma once
#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

// FirstPersonController — simple free-look movement for a camera-bearing player.
// It uses the owning object's yaw/pitch transform and supports WASD movement,
// mouse look, and optional sprint.
class FirstPersonController : public Engine::Core::Script
{
public:
    FirstPersonController();
    ~FirstPersonController() override;

    PROPERTY(Inspector, EditAnywhere, Category = "Movement")
    float moveSpeed = 4.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Movement")
    float sprintMultiplier = 1.6f;

    PROPERTY(Inspector, EditAnywhere, Category = "Movement")
    float lookSensitivity = 0.0025f;

    PROPERTY(Inspector, EditAnywhere, Category = "Movement")
    bool invertY = false;

    PROPERTY(Inspector, EditAnywhere, Category = "Movement")
    bool lockCursor = true;

    // Free flight for exploring large maps: no gravity, Space climbs, Shift
    // descends, Ctrl boosts by sprintMultiplier. Velocity eases toward the
    // input (flightResponse per second) for smooth, glide-like motion.
    PROPERTY(Inspector, EditAnywhere, Category = "Flight")
    bool flightMode = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Flight", ClampMin = "0.5", ClampMax = "30")
    float flightResponse = 3.f;
    // Kept above terrain (any TerrainManager in the scene) so fast flight
    // cannot pass through ground whose colliders have not streamed in yet.
    PROPERTY(Inspector, EditAnywhere, Category = "Flight", ClampMin = "0")
    float flightGroundClearance = 1.5f;
    // Flight never goes below this world height (for example just above a
    // water surface). Very low values disable it.
    PROPERTY(Inspector, EditAnywhere, Category = "Flight")
    float flightMinimumHeight = -1.e9f;

    void Start() override;
    void Update() override;
    void Disabled() override;

private:
    void UpdateLook();
    void UpdateMovement(float deltaTime);
    void SetCursorLock(bool locked);
    bool IsApplicationFocused() const;
    bool GetLookAnchor(POINT& anchor) const;
    static bool IsKeyDown(int virtualKey);

    bool m_cursorLocked = false;
    bool m_inputSuspended = false;
    bool m_leftMouseWasDown = false;
    bool m_hasLastCursorPosition = false;
    POINT m_lastCursorPosition{};
    bool m_hasLookForward = false;
    glm::vec3 m_lookForward { 0.f, 0.f, 1.f };
    bool m_hasBodyFrame = false;
    glm::mat3 m_lastBodyBasis { 1.f };
    glm::vec3 m_flightVelocity { 0.f };
    void UpdateFlight(float deltaTime);
};
