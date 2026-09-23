#include "Scripts/Demos/Pong/PongBall.h"

#include "Scripts/Demos/Pong/PongGameManager.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

PongBall::PongBall()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongBall));
    RegisterField("managerObjectName", managerObjectName);
    RegisterField("leftPaddleName", leftPaddleName);
    RegisterField("rightPaddleName", rightPaddleName);
    RegisterField("speed", speed);
    RegisterField("speedGainPerHit", speedGainPerHit);
    RegisterField("maximumSpeed", maximumSpeed);
    RegisterField("serveDelay", serveDelay);
    RegisterField("fieldHalfWidth", fieldHalfWidth);
    RegisterField("fieldHalfHeight", fieldHalfHeight);
}

namespace
{
struct PongBallRegistration
{
    PongBallRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongBall>("PongBall");
    }
};
PongBallRegistration g_registration;
}

void PongBall::Start()
{
    ResolveObjects();
}

void PongBall::Update()
{
    if (!Owner || !Owner->GetScene())
        return;
    if (!m_manager || !m_leftPaddle || !m_rightPaddle)
        ResolveObjects();
    if (!m_manager || !m_leftPaddle || !m_rightPaddle ||
        !m_manager->IsPlaying())
        return;

    const float deltaTime = std::clamp(Owner->GetScene()->GetDeltaTime(),
        0.f, 0.05f);
    if (m_serveTimer > 0.f)
    {
        m_serveTimer = std::max(0.f, m_serveTimer - deltaTime);
        if (m_serveTimer == 0.f)
            LaunchServe();
        return;
    }

    const glm::vec2 ballHalf = ColliderHalfSize(Owner, glm::vec2(0.16f));
    const glm::vec2 leftHalf = ColliderHalfSize(m_leftPaddle,
        glm::vec2(0.18f, 0.8f));
    const glm::vec2 rightHalf = ColliderHalfSize(m_rightPaddle,
        glm::vec2(0.18f, 0.8f));
    glm::vec3 next = Owner->transform.position;
    next.x += m_velocity.x * deltaTime;
    next.y += m_velocity.y * deltaTime;

    if (next.y + ballHalf.y >= fieldHalfHeight && m_velocity.y > 0.f)
    {
        next.y = fieldHalfHeight - ballHalf.y;
        m_velocity.y = -std::abs(m_velocity.y);
    }
    else if (next.y - ballHalf.y <= -fieldHalfHeight && m_velocity.y < 0.f)
    {
        next.y = -fieldHalfHeight + ballHalf.y;
        m_velocity.y = std::abs(m_velocity.y);
    }

    const glm::vec3 leftPosition = m_leftPaddle->transform.position;
    const float leftFace = leftPosition.x + leftHalf.x;
    if (m_velocity.x < 0.f && next.x - ballHalf.x <= leftFace &&
        Owner->transform.position.x - ballHalf.x >= leftFace &&
        std::abs(next.y - leftPosition.y) <= leftHalf.y + ballHalf.y)
    {
        Owner->transform.position = next;
        Owner->transform.position.x = leftFace + ballHalf.x;
        BounceFromPaddle(*m_leftPaddle, 1.f, leftHalf.y);
        next = Owner->transform.position;
    }

    const glm::vec3 rightPosition = m_rightPaddle->transform.position;
    const float rightFace = rightPosition.x - rightHalf.x;
    if (m_velocity.x > 0.f && next.x + ballHalf.x >= rightFace &&
        Owner->transform.position.x + ballHalf.x <= rightFace &&
        std::abs(next.y - rightPosition.y) <= rightHalf.y + ballHalf.y)
    {
        Owner->transform.position = next;
        Owner->transform.position.x = rightFace - ballHalf.x;
        BounceFromPaddle(*m_rightPaddle, -1.f, rightHalf.y);
        next = Owner->transform.position;
    }

    Owner->transform.position = next;
    if (next.x + ballHalf.x < -fieldHalfWidth)
        m_manager->ScorePoint(false);
    else if (next.x - ballHalf.x > fieldHalfWidth)
        m_manager->ScorePoint(true);
}

void PongBall::ResolveObjects()
{
    Engine::Core::Object* manager = FindObjectInSceneByName(managerObjectName);
    m_manager = manager ? manager->GetComponent<PongGameManager>() : nullptr;
    m_leftPaddle = FindObjectInSceneByName(leftPaddleName);
    m_rightPaddle = FindObjectInSceneByName(rightPaddleName);
}

void PongBall::ResetRound(float horizontalDirection)
{
    if (!m_manager)
        ResolveObjects();
    HoldAtCenter();
    m_pendingServeDirection = horizontalDirection < 0.f ? -1.f : 1.f;
    m_serveTimer = std::max(0.f, serveDelay);
    if (m_serveTimer == 0.f)
        LaunchServe();
    else if (m_manager)
        m_manager->SetStatus("GET READY");
}

void PongBall::HoldAtCenter()
{
    if (Owner)
        Owner->transform.position = glm::vec3(0.f, 0.f,
            Owner->transform.position.z);
    m_velocity = glm::vec2(0.f);
    m_serveTimer = 0.f;
}

void PongBall::LaunchServe()
{
    const float verticalDirection = (m_serveIndex++ % 2 == 0)
        ? 0.38f : -0.38f;
    m_velocity = glm::normalize(glm::vec2(
        m_pendingServeDirection, verticalDirection)) * speed;
    if (m_manager)
        m_manager->SetStatus("");
}

void PongBall::BounceFromPaddle(Engine::Core::Object& paddle,
    float horizontalDirection, float paddleHalfHeight)
{
    const float offset = std::clamp(
        (Owner->transform.position.y - paddle.transform.position.y) /
            std::max(paddleHalfHeight, 0.01f), -1.f, 1.f);
    const float angle = offset * glm::radians(60.f);
    const float nextSpeed = std::min(maximumSpeed,
        std::max(speed, glm::length(m_velocity) + speedGainPerHit));
    m_velocity = glm::vec2(horizontalDirection * std::cos(angle),
        std::sin(angle)) * nextSpeed;
}

glm::vec2 PongBall::ColliderHalfSize(const Engine::Core::Object* object,
    const glm::vec2& fallback)
{
    if (!object)
        return fallback;
    const auto* collider =
        object->GetComponent<Engine::Components::PrimitiveObjectCollider>();
    if (!collider)
        return fallback;
    const glm::vec3 scale = glm::abs(object->transform.scale);
    return glm::max(glm::vec2(collider->size.x * scale.x,
        collider->size.y * scale.y) * 0.5f, glm::vec2(0.001f));
}
