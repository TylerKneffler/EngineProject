#define GLM_ENABLE_EXPERIMENTAL
#include "Scripts/FirstPersonController.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <chrono>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

namespace
{
constexpr float kMaxPitch = 1.55f;

glm::vec3 ForwardVector(float yaw)
{
    return glm::normalize(glm::vec3(-std::sin(yaw), 0.f, std::cos(yaw)));
}

glm::vec3 RightVector(float yaw)
{
    return glm::normalize(glm::vec3(std::cos(yaw), 0.f, std::sin(yaw)));
}

float FrameDeltaSeconds(const std::chrono::steady_clock::time_point& lastFrame)
{
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - lastFrame).count();
    return (delta > 0.f && delta < 0.25f) ? delta : (1.f / 60.f);
}
}

FirstPersonController::FirstPersonController()
{
    SetTypeName(COMPONENT_TYPE_NAME(FirstPersonController));
    RegisterField("moveSpeed", moveSpeed);
    RegisterField("sprintMultiplier", sprintMultiplier);
    RegisterField("lookSensitivity", lookSensitivity);
    RegisterField("invertY", invertY);
    RegisterField("lockCursor", lockCursor);
}

namespace
{
struct FirstPersonControllerRegistration
{
    FirstPersonControllerRegistration()
    {
        Engine::Serialization::RegisterComponentType<FirstPersonController>("FirstPersonController");
    }
};

FirstPersonControllerRegistration g_registration;
}

void FirstPersonController::Start()
{
    m_yaw = Owner ? Owner->transform.rotation.y : 0.f;
    m_pitch = Owner ? Owner->transform.rotation.x : 0.f;
    m_lastFrame = std::chrono::steady_clock::now();
    SetCursorLock(lockCursor);
}

void FirstPersonController::Update()
{
    if (!Owner)
        return;

    const float dt = FrameDeltaSeconds(m_lastFrame);
    m_lastFrame = std::chrono::steady_clock::now();
    UpdateLook();
    UpdateMovement(dt);
}

void FirstPersonController::UpdateLook()
{
    if (!m_cursorLocked || !Owner)
        return;

    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return;

    RECT rect{};
    if (!GetClientRect(foreground, &rect))
        return;

    POINT center{ rect.right / 2, rect.bottom / 2 };
    ClientToScreen(foreground, &center);

    POINT cursor{};
    GetCursorPos(&cursor);

    const float deltaX = static_cast<float>(cursor.x - center.x);
    const float deltaY = static_cast<float>(cursor.y - center.y);
    if (deltaX == 0.f && deltaY == 0.f)
        return;

    m_yaw -= deltaX * lookSensitivity;
    m_pitch -= (invertY ? -1.f : 1.f) * deltaY * lookSensitivity;
    m_pitch = std::clamp(m_pitch, -kMaxPitch, kMaxPitch);

    Owner->transform.rotation = { m_pitch, m_yaw, 0.f };
    SetCursorPos(center.x, center.y);
}

void FirstPersonController::UpdateMovement(float deltaTime)
{
    if (!Owner)
        return;

    const float moveX = static_cast<float>(IsKeyDown('D') || IsKeyDown(VK_RIGHT)) -
        static_cast<float>(IsKeyDown('A') || IsKeyDown(VK_LEFT));
    const float moveZ = static_cast<float>(IsKeyDown('W') || IsKeyDown(VK_UP)) -
        static_cast<float>(IsKeyDown('S') || IsKeyDown(VK_DOWN));
    const float sprint = (IsKeyDown(VK_SHIFT) ? sprintMultiplier : 1.f);

    const glm::vec3 forward = ForwardVector(m_yaw);
    const glm::vec3 right = RightVector(m_yaw);
    const glm::vec3 movement = (forward * moveZ + right * moveX);

    const float speed = moveSpeed * sprint * deltaTime;
    if (glm::length2(movement) > 0.0001f)
    {
        const glm::vec3 direction = glm::normalize(movement);
        const glm::vec3 worldDelta = direction * speed;
        Owner->transform.position += worldDelta;

        if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
        {
            glm::vec3 velocity = body->GetLinearVelocity();
            velocity.x = direction.x * moveSpeed * sprint;
            velocity.z = direction.z * moveSpeed * sprint;
            body->SetLinearVelocity(velocity);
        }
    }
    else if (auto* body = Owner->GetComponent<Engine::Components::RigidBody>())
    {
        glm::vec3 velocity = body->GetLinearVelocity();
        velocity.x = 0.f;
        velocity.z = 0.f;
        body->SetLinearVelocity(velocity);
    }
}

void FirstPersonController::SetCursorLock(bool locked)
{
    m_cursorLocked = locked;
    if (locked)
    {
        ShowCursor(FALSE);
    }
    else
    {
        ShowCursor(TRUE);
    }
}

bool FirstPersonController::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}
