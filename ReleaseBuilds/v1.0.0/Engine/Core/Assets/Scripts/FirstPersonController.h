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

    void Start() override;
    void Update() override;

private:
    void UpdateLook();
    void UpdateMovement(float deltaTime);
    void SetCursorLock(bool locked);
    static bool IsKeyDown(int virtualKey);

    float m_yaw = 0.f;
    float m_pitch = 0.f;
    bool m_cursorLocked = false;
    std::chrono::steady_clock::time_point m_lastFrame;
};
