#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

namespace Engine::Components
{
class PrimitiveObjectCollider;
class UIText;
}

// Coordinates the classic Pong demo using ordinary scene objects and
// components. Collider sizes are the source of truth for gameplay contacts,
// while rendering remains independent through Mesh and Material components.
class PongGameController final : public Engine::Core::Script
{
public:
    PongGameController();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string leftPaddleName = "Left Paddle";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string rightPaddleName = "Right Paddle";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string ballName = "Ball";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string scoreTextName = "Score";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string statusTextName = "Status";

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "0.1")
    float paddleSpeed = 7.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "0.1")
    float ballSpeed = 6.4f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "0")
    float speedGainPerHit = 0.35f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "0.1")
    float maximumBallSpeed = 11.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "1")
    int winningScore = 11;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay")
    bool rightPaddleAi = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Gameplay", ClampMin = "0.1")
    float aiSpeed = 6.2f;

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfWidth = 8.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfHeight = 4.35f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "0")
    float serveDelay = 0.8f;

    void Start() override;
    void Update() override;

private:
    void ResolveObjects();
    void ResetMatch();
    void ResetRound(float horizontalDirection);
    void UpdatePaddles(float deltaTime);
    void UpdateBall(float deltaTime);
    void BounceFromPaddle(Engine::Core::Object& paddle,
        float horizontalDirection, float paddleHalfHeight);
    void ScorePoint(bool leftPlayerScored);
    void RefreshHud();
    static bool IsKeyDown(int virtualKey);
    static glm::vec2 ColliderHalfSize(const Engine::Core::Object* object,
        const glm::vec2& fallback);

    Engine::Core::Object* m_leftPaddle = nullptr;
    Engine::Core::Object* m_rightPaddle = nullptr;
    Engine::Core::Object* m_ball = nullptr;
    Engine::Components::UIText* m_scoreText = nullptr;
    Engine::Components::UIText* m_statusText = nullptr;
    glm::vec2 m_ballVelocity { 0.f };
    float m_serveTimer = 0.f;
    float m_pendingServeDirection = 1.f;
    int m_leftScore = 0;
    int m_rightScore = 0;
    int m_serveIndex = 0;
    bool m_matchOver = false;
    bool m_restartWasDown = false;
};
