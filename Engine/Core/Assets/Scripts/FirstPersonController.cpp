#define GLM_ENABLE_EXPERIMENTAL
#include "Scripts/FirstPersonController.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <chrono>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/norm.hpp>

namespace
{
constexpr float kMaxPitch = 1.55f;

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > 1e-8f ? value / std::sqrt(lengthSquared) : fallback;
}

glm::vec3 PerpendicularForward(const glm::vec3& up)
{
    const glm::vec3 reference = std::abs(up.z) < 0.9f
        ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(1.f, 0.f, 0.f);
    return SafeNormalize(reference - up * glm::dot(reference, up),
        glm::vec3(1.f, 0.f, 0.f));
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

    auto* body = Owner->GetComponent<Engine::Components::RigidBody>();
    const glm::vec3 gravityDown = body
        ? body->GetGravityDirection() : glm::vec3(0.f, -1.f, 0.f);
    const glm::vec3 gravityUp = -gravityDown;
    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    glm::vec3 forward = SafeNormalize(glm::vec3(ownerWorld[2]),
        PerpendicularForward(gravityUp));

    forward = SafeNormalize(glm::angleAxis(-deltaX * lookSensitivity,
        gravityUp) * forward, forward);
    glm::vec3 right = SafeNormalize(glm::cross(gravityUp, forward),
        SafeNormalize(glm::vec3(ownerWorld[0]),
            glm::cross(gravityUp, PerpendicularForward(gravityUp))));

    const float currentPitch = std::asin(std::clamp(
        glm::dot(forward, gravityUp), -1.f, 1.f));
    const float pitchDelta = -(invertY ? -1.f : 1.f) * deltaY *
        lookSensitivity;
    const float targetPitch = std::clamp(currentPitch + pitchDelta,
        -kMaxPitch, kMaxPitch);
    forward = SafeNormalize(glm::angleAxis(targetPitch - currentPitch, right) *
        forward, forward);
    right = SafeNormalize(glm::cross(gravityUp, forward), right);
    const glm::vec3 cameraUp = SafeNormalize(glm::cross(forward, right),
        gravityUp);

    glm::mat3 worldBasis(1.f);
    worldBasis[0] = right;
    worldBasis[1] = cameraUp;
    worldBasis[2] = forward;
    const glm::quat worldRotation = glm::normalize(glm::quat_cast(worldBasis));
    if (body)
        body->SetWorldPose(Owner->transform.GetWorldPosition(), worldRotation);
    else
        Owner->transform.rotation = glm::eulerAngles(worldRotation);
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

    auto* body = Owner->GetComponent<Engine::Components::RigidBody>();
    const glm::vec3 gravityDown = body
        ? body->GetGravityDirection() : glm::vec3(0.f, -1.f, 0.f);
    const glm::vec3 gravityUp = -gravityDown;
    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 rawForward = glm::vec3(ownerWorld[2]);
    const glm::vec3 forward = SafeNormalize(rawForward - gravityUp *
        glm::dot(rawForward, gravityUp), PerpendicularForward(gravityUp));
    const glm::vec3 right = SafeNormalize(glm::cross(gravityUp, forward),
        glm::cross(gravityUp, PerpendicularForward(gravityUp)));
    const glm::vec3 movement = (forward * moveZ + right * moveX);

    if (glm::length2(movement) > 0.0001f)
    {
        const glm::vec3 direction = glm::normalize(movement);
        if (body)
        {
            glm::vec3 velocity = body->GetLinearVelocity();
            const glm::vec3 gravityVelocity = gravityDown *
                glm::dot(velocity, gravityDown);
            velocity = gravityVelocity + direction * moveSpeed * sprint;
            body->SetLinearVelocity(velocity);
        }
        else
        {
            Owner->transform.position += direction * moveSpeed * sprint * deltaTime;
        }
    }
    else if (body)
    {
        glm::vec3 velocity = body->GetLinearVelocity();
        velocity = gravityDown * glm::dot(velocity, gravityDown);
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
