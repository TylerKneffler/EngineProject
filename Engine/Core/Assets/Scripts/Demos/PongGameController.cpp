#include "Scripts/Demos/PongGameController.h"

#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

PongGameController::PongGameController()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongGameController));
    RegisterField("leftPaddleName", leftPaddleName);
    RegisterField("rightPaddleName", rightPaddleName);
    RegisterField("ballName", ballName);
    RegisterField("scoreTextName", scoreTextName);
    RegisterField("statusTextName", statusTextName);
    RegisterField("paddleSpeed", paddleSpeed);
    RegisterField("ballSpeed", ballSpeed);
    RegisterField("speedGainPerHit", speedGainPerHit);
    RegisterField("maximumBallSpeed", maximumBallSpeed);
    RegisterField("winningScore", winningScore);
    RegisterField("rightPaddleAi", rightPaddleAi);
    RegisterField("aiSpeed", aiSpeed);
    RegisterField("fieldHalfWidth", fieldHalfWidth);
    RegisterField("fieldHalfHeight", fieldHalfHeight);
    RegisterField("serveDelay", serveDelay);
}

namespace
{
struct PongGameControllerRegistration
{
    PongGameControllerRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongGameController>(
            "PongGameController");
    }
};

PongGameControllerRegistration g_registration;
}

void PongGameController::Start()
{
    ResolveObjects();
    ResetMatch();
}

void PongGameController::Update()
{
    if (!Owner || !Owner->GetScene())
        return;
    if (!m_leftPaddle || !m_rightPaddle || !m_ball)
        ResolveObjects();
    if (!m_leftPaddle || !m_rightPaddle || !m_ball)
        return;

    const float deltaTime = std::clamp(Owner->GetScene()->GetDeltaTime(),
        0.f, 0.05f);
    const bool restartDown = IsKeyDown(VK_SPACE) || IsKeyDown(VK_RETURN);
    if (m_matchOver)
    {
        if (restartDown && !m_restartWasDown)
            ResetMatch();
        m_restartWasDown = restartDown;
        return;
    }
    m_restartWasDown = restartDown;

    UpdatePaddles(deltaTime);
    if (m_serveTimer > 0.f)
    {
        m_serveTimer = std::max(0.f, m_serveTimer - deltaTime);
        if (m_serveTimer == 0.f)
        {
            const float verticalDirection = (m_serveIndex++ % 2 == 0)
                ? 0.38f : -0.38f;
            m_ballVelocity = glm::normalize(glm::vec2(
                m_pendingServeDirection, verticalDirection)) * ballSpeed;
            if (m_statusText)
            {
                m_statusText->text.clear();
                m_statusText->MarkConfigurationDirty();
            }
        }
        return;
    }
    UpdateBall(deltaTime);
}

void PongGameController::ResolveObjects()
{
    m_leftPaddle = FindObjectInSceneByName(leftPaddleName);
    m_rightPaddle = FindObjectInSceneByName(rightPaddleName);
    m_ball = FindObjectInSceneByName(ballName);
    Engine::Core::Object* scoreObject =
        FindObjectInSceneByName(scoreTextName);
    Engine::Core::Object* statusObject =
        FindObjectInSceneByName(statusTextName);
    m_scoreText = scoreObject
        ? scoreObject->GetComponent<Engine::Components::UIText>() : nullptr;
    m_statusText = statusObject
        ? statusObject->GetComponent<Engine::Components::UIText>() : nullptr;
}

void PongGameController::ResetMatch()
{
    m_leftScore = 0;
    m_rightScore = 0;
    m_matchOver = false;
    m_serveIndex = 0;
    if (m_leftPaddle)
        m_leftPaddle->transform.position.y = 0.f;
    if (m_rightPaddle)
        m_rightPaddle->transform.position.y = 0.f;
    ResetRound(1.f);
    RefreshHud();
}

void PongGameController::ResetRound(float horizontalDirection)
{
    if (m_ball)
        m_ball->transform.position = glm::vec3(0.f, 0.f,
            m_ball->transform.position.z);
    m_ballVelocity = glm::vec2(0.f);
    m_pendingServeDirection = horizontalDirection < 0.f ? -1.f : 1.f;
    m_serveTimer = std::max(0.f, serveDelay);
    if (m_serveTimer == 0.f)
    {
        const float verticalDirection = (m_serveIndex++ % 2 == 0)
            ? 0.38f : -0.38f;
        m_ballVelocity = glm::normalize(glm::vec2(
            m_pendingServeDirection, verticalDirection)) * ballSpeed;
    }
    if (m_statusText)
    {
        m_statusText->text = m_serveTimer > 0.f ? "GET READY" : "";
        m_statusText->MarkConfigurationDirty();
    }
}

void PongGameController::UpdatePaddles(float deltaTime)
{
    const glm::vec2 leftHalf = ColliderHalfSize(m_leftPaddle,
        glm::vec2(0.18f, 0.8f));
    const glm::vec2 rightHalf = ColliderHalfSize(m_rightPaddle,
        glm::vec2(0.18f, 0.8f));
    const float leftInput = static_cast<float>(IsKeyDown('W')) -
        static_cast<float>(IsKeyDown('S'));
    m_leftPaddle->transform.position.y = std::clamp(
        m_leftPaddle->transform.position.y + leftInput * paddleSpeed * deltaTime,
        -fieldHalfHeight + leftHalf.y, fieldHalfHeight - leftHalf.y);

    float rightInput = 0.f;
    if (rightPaddleAi)
    {
        const float target = m_ballVelocity.x > 0.f
            ? m_ball->transform.position.y : 0.f;
        const float difference = target - m_rightPaddle->transform.position.y;
        if (std::abs(difference) > 0.08f)
            rightInput = difference > 0.f ? 1.f : -1.f;
    }
    else
    {
        rightInput = static_cast<float>(IsKeyDown(VK_UP)) -
            static_cast<float>(IsKeyDown(VK_DOWN));
    }
    const float rightSpeed = rightPaddleAi ? aiSpeed : paddleSpeed;
    m_rightPaddle->transform.position.y = std::clamp(
        m_rightPaddle->transform.position.y + rightInput * rightSpeed * deltaTime,
        -fieldHalfHeight + rightHalf.y, fieldHalfHeight - rightHalf.y);
}

void PongGameController::UpdateBall(float deltaTime)
{
    const glm::vec2 ballHalf = ColliderHalfSize(m_ball,
        glm::vec2(0.16f));
    const glm::vec2 leftHalf = ColliderHalfSize(m_leftPaddle,
        glm::vec2(0.18f, 0.8f));
    const glm::vec2 rightHalf = ColliderHalfSize(m_rightPaddle,
        glm::vec2(0.18f, 0.8f));
    glm::vec3 next = m_ball->transform.position;
    next.x += m_ballVelocity.x * deltaTime;
    next.y += m_ballVelocity.y * deltaTime;

    if (next.y + ballHalf.y >= fieldHalfHeight && m_ballVelocity.y > 0.f)
    {
        next.y = fieldHalfHeight - ballHalf.y;
        m_ballVelocity.y = -std::abs(m_ballVelocity.y);
    }
    else if (next.y - ballHalf.y <= -fieldHalfHeight &&
        m_ballVelocity.y < 0.f)
    {
        next.y = -fieldHalfHeight + ballHalf.y;
        m_ballVelocity.y = std::abs(m_ballVelocity.y);
    }

    const glm::vec3 leftPosition = m_leftPaddle->transform.position;
    const float leftFace = leftPosition.x + leftHalf.x;
    if (m_ballVelocity.x < 0.f &&
        next.x - ballHalf.x <= leftFace &&
        m_ball->transform.position.x - ballHalf.x >= leftFace &&
        std::abs(next.y - leftPosition.y) <= leftHalf.y + ballHalf.y)
    {
        m_ball->transform.position = next;
        m_ball->transform.position.x = leftFace + ballHalf.x;
        BounceFromPaddle(*m_leftPaddle, 1.f, leftHalf.y);
        next = m_ball->transform.position;
    }

    const glm::vec3 rightPosition = m_rightPaddle->transform.position;
    const float rightFace = rightPosition.x - rightHalf.x;
    if (m_ballVelocity.x > 0.f &&
        next.x + ballHalf.x >= rightFace &&
        m_ball->transform.position.x + ballHalf.x <= rightFace &&
        std::abs(next.y - rightPosition.y) <= rightHalf.y + ballHalf.y)
    {
        m_ball->transform.position = next;
        m_ball->transform.position.x = rightFace - ballHalf.x;
        BounceFromPaddle(*m_rightPaddle, -1.f, rightHalf.y);
        next = m_ball->transform.position;
    }

    m_ball->transform.position = next;
    if (next.x + ballHalf.x < -fieldHalfWidth)
        ScorePoint(false);
    else if (next.x - ballHalf.x > fieldHalfWidth)
        ScorePoint(true);
}

void PongGameController::BounceFromPaddle(Engine::Core::Object& paddle,
    float horizontalDirection, float paddleHalfHeight)
{
    const float offset = std::clamp(
        (m_ball->transform.position.y - paddle.transform.position.y) /
            std::max(paddleHalfHeight, 0.01f),
        -1.f, 1.f);
    const float angle = offset * glm::radians(60.f);
    const float speed = std::min(maximumBallSpeed,
        std::max(ballSpeed, glm::length(m_ballVelocity) + speedGainPerHit));
    m_ballVelocity = glm::vec2(horizontalDirection * std::cos(angle),
        std::sin(angle)) * speed;
}

void PongGameController::ScorePoint(bool leftPlayerScored)
{
    if (leftPlayerScored)
        ++m_leftScore;
    else
        ++m_rightScore;
    RefreshHud();

    if (m_leftScore >= std::max(1, winningScore) ||
        m_rightScore >= std::max(1, winningScore))
    {
        m_matchOver = true;
        m_ballVelocity = glm::vec2(0.f);
        m_ball->transform.position.x = 0.f;
        m_ball->transform.position.y = 0.f;
        if (m_statusText)
        {
            m_statusText->text = m_leftScore > m_rightScore
                ? "PLAYER 1 WINS  -  SPACE TO RESTART"
                : "PLAYER 2 WINS  -  SPACE TO RESTART";
            m_statusText->MarkConfigurationDirty();
        }
        return;
    }
    ResetRound(leftPlayerScored ? 1.f : -1.f);
}

void PongGameController::RefreshHud()
{
    if (!m_scoreText)
        return;
    m_scoreText->text = std::to_string(m_leftScore) + "     " +
        std::to_string(m_rightScore);
    m_scoreText->MarkConfigurationDirty();
}

bool PongGameController::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

glm::vec2 PongGameController::ColliderHalfSize(
    const Engine::Core::Object* object, const glm::vec2& fallback)
{
    if (!object)
        return fallback;
    const auto* collider =
        object->GetComponent<Engine::Components::PrimitiveObjectCollider>();
    if (!collider)
        return fallback;
    const glm::vec3 worldScale = glm::abs(object->transform.scale);
    return glm::max(glm::vec2(collider->size.x * worldScale.x,
        collider->size.y * worldScale.y) * 0.5f, glm::vec2(0.001f));
}
