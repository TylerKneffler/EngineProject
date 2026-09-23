#define GLM_ENABLE_EXPERIMENTAL
#include "Scripts/Controllers/FirstPersonController.h"
#include "Core/Compoonents/Camera/Camera.h"
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

void SetSystemCursorVisible(bool visible)
{
    CURSORINFO cursorInfo { sizeof(CURSORINFO) };
    if (GetCursorInfo(&cursorInfo) &&
        ((cursorInfo.flags & CURSOR_SHOWING) != 0) == visible)
    {
        return;
    }
    if (visible)
    {
        while (ShowCursor(TRUE) < 0)
        {
        }
    }
    else
    {
        while (ShowCursor(FALSE) >= 0)
        {
        }
    }
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
    m_hasLookForward = false;
    m_hasBodyFrame = false;
    const bool focused = IsApplicationFocused();
    m_inputSuspended = !focused;
    m_leftMouseWasDown = IsKeyDown(VK_LBUTTON);
    SetCursorLock(lockCursor && focused);
}

void FirstPersonController::Update()
{
    if (!Owner)
        return;

    const float dt = FrameDeltaSeconds(m_lastFrame);
    m_lastFrame = std::chrono::steady_clock::now();

    const bool focused = IsApplicationFocused();
    // Read both bits in one call. The transition bit preserves a quick
    // down/up click even when the window message queue is fully drained before
    // this update, while the high bit still tracks a held button.
    const SHORT leftMouseState = GetAsyncKeyState(VK_LBUTTON);
    const bool leftMouseDown = (leftMouseState & 0x8000) != 0;
    const bool leftMousePressed = (leftMouseState & 0x0001) != 0;
    if (!focused || IsKeyDown(VK_ESCAPE))
    {
        m_inputSuspended = true;
        SetCursorLock(false);
        m_leftMouseWasDown = leftMouseDown;
        return;
    }

    if (m_inputSuspended)
    {
        // Focus loss and Escape deliberately require a fresh click before the
        // controller can own the pointer again. This prevents Alt+Tab or the
        // Windows key from immediately snapping the cursor back on return.
        const bool clickedAfterRelease = leftMousePressed ||
            (leftMouseDown && !m_leftMouseWasDown);
        m_leftMouseWasDown = leftMouseDown;
        if (!clickedAfterRelease)
            return;
        m_inputSuspended = false;
        SetCursorLock(lockCursor);
    }
    else
    {
        m_leftMouseWasDown = leftMouseDown;
    }

    UpdateLook();
    if (m_inputSuspended)
        return;
    UpdateMovement(dt);
}

void FirstPersonController::UpdateLook()
{
    if (!m_cursorLocked || !Owner)
        return;

    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return;

    RECT windowRect{};
    if (!GetWindowRect(foreground, &windowRect))
        return;

    POINT cursor{};
    if (!GetCursorPos(&cursor))
        return;

    // Keep the pointer hidden while it is over the editor and reveal it after
    // it crosses the window edge. Merely crossing the edge must not release
    // gameplay focus: the user may move back in without interrupting control.
    // A click or other activation outside the editor changes native focus;
    // the focused check at the start of Update then suspends gameplay.
    const bool pointerInsideEditor = PtInRect(&windowRect, cursor) != FALSE;
    SetSystemCursorVisible(!pointerInsideEditor);

    if (!m_hasLastCursorPosition)
    {
        m_lastCursorPosition = cursor;
        m_hasLastCursorPosition = true;
        return;
    }

    const float deltaX = static_cast<float>(cursor.x - m_lastCursorPosition.x);
    const float deltaY = static_cast<float>(cursor.y - m_lastCursorPosition.y);
    m_lastCursorPosition = cursor;

    auto* body = Owner->GetComponent<Engine::Components::RigidBody>();
    const glm::vec3 gravityDown = body
        ? body->GetGravityDirection() : glm::vec3(0.f, -1.f, 0.f);
    const glm::vec3 gravityUp = -gravityDown;
    const glm::mat4 ownerWorld = Owner->transform.GetWorldMatrix();
    const glm::vec3 worldPosition = glm::vec3(ownerWorld[3]);
    Engine::Components::Camera* camera =
        Owner->GetComponent<Engine::Components::Camera>();
    if (!m_hasLookForward)
    {
        m_lookForward = camera && !camera->useTransformRotation
            ? SafeNormalize(camera->target - worldPosition,
                SafeNormalize(glm::vec3(ownerWorld[2]),
                    PerpendicularForward(gravityUp)))
            : SafeNormalize(glm::vec3(ownerWorld[2]),
                PerpendicularForward(gravityUp));
        m_hasLookForward = true;
    }

    // Portal traversal intentionally remaps the body's whole gravity frame
    // after controller update. Carry that external frame change into the
    // stored view direction on the following tick. Ordinary collision
    // impulses cannot enter this path because the capsule's rotation is
    // locked and the controller publishes the same upright basis each frame.
    if (body)
    {
        const glm::vec3 currentBodyForward = SafeNormalize(
            glm::vec3(ownerWorld[2]) - gravityUp *
                glm::dot(glm::vec3(ownerWorld[2]), gravityUp),
            PerpendicularForward(gravityUp));
        const glm::vec3 currentBodyRight = SafeNormalize(
            glm::cross(gravityUp, currentBodyForward),
            glm::cross(gravityUp, PerpendicularForward(gravityUp)));
        glm::mat3 currentBodyBasis(1.f);
        currentBodyBasis[0] = currentBodyRight;
        currentBodyBasis[1] = gravityUp;
        currentBodyBasis[2] = currentBodyForward;
        if (m_hasBodyFrame)
            m_lookForward = SafeNormalize(
                currentBodyBasis * glm::transpose(m_lastBodyBasis) *
                    m_lookForward,
                currentBodyForward);
    }
    glm::vec3 forward = m_lookForward;

    if (deltaX != 0.f)
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
    if (deltaY != 0.f)
        forward = SafeNormalize(glm::angleAxis(targetPitch - currentPitch, right) *
            forward, forward);
    right = SafeNormalize(glm::cross(gravityUp, forward), right);
    const glm::vec3 cameraUp = SafeNormalize(glm::cross(forward, right),
        gravityUp);
    m_lookForward = forward;

    // Pitch changes the view only. Keep the physical capsule aligned to its
    // gravity frame so stair and ledge contacts cannot roll or spin it.
    const glm::vec3 bodyForward = SafeNormalize(forward - gravityUp *
        glm::dot(forward, gravityUp), PerpendicularForward(gravityUp));
    const glm::vec3 bodyRight = SafeNormalize(
        glm::cross(gravityUp, bodyForward), right);
    glm::mat3 worldBasis(1.f);
    worldBasis[0] = bodyRight;
    worldBasis[1] = gravityUp;
    worldBasis[2] = bodyForward;
    const glm::quat worldRotation = glm::normalize(glm::quat_cast(worldBasis));
    if (body)
    {
        body->SetWorldPose(worldPosition, worldRotation);
        body->SetAngularVelocity(glm::vec3(0.f));
        m_lastBodyBasis = worldBasis;
        m_hasBodyFrame = true;
    }
    else
        Owner->transform.rotation = glm::eulerAngles(worldRotation);

    if (camera)
    {
        camera->useTransformRotation = false;
        // Camera::target is a world-space point. Keep it distant so movement
        // performed later in the physics tick cannot noticeably bend the view.
        camera->target = worldPosition + forward * 1000.f;
        camera->up = cameraUp;
    }
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
    if (m_cursorLocked == locked)
        return;
    m_cursorLocked = locked;
    if (locked)
    {
        m_hasLastCursorPosition = GetCursorPos(&m_lastCursorPosition) != FALSE;
        SetSystemCursorVisible(false);
    }
    else
    {
        m_hasLastCursorPosition = false;
        SetSystemCursorVisible(true);
    }
}

bool FirstPersonController::IsApplicationFocused()
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !GetFocus())
        return false;

    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(foreground, &foregroundProcess);
    return foregroundProcess == GetCurrentProcessId();
}

bool FirstPersonController::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}
