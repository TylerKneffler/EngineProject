#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <string>

class PongGameManager;

class PongBall final : public Engine::Core::Script
{
public:
    PongBall();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string managerObjectName = "Pong Game";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string leftPaddleName = "Left Paddle";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Objects")
    std::string rightPaddleName = "Right Paddle";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Ball", ClampMin = "0.1")
    float speed = 6.4f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Ball", ClampMin = "0")
    float speedGainPerHit = 0.35f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Ball", ClampMin = "0.1")
    float maximumSpeed = 11.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Ball", ClampMin = "0")
    float serveDelay = 0.8f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfWidth = 8.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfHeight = 4.35f;

    void Start() override;
    void Update() override;
    void ResetRound(float horizontalDirection);
    void HoldAtCenter();

private:
    void ResolveObjects();
    void LaunchServe();
    void BounceFromPaddle(Engine::Core::Object& paddle,
        float horizontalDirection, float paddleHalfHeight);
    static glm::vec2 ColliderHalfSize(const Engine::Core::Object* object,
        const glm::vec2& fallback);

    PongGameManager* m_manager = nullptr;
    Engine::Core::Object* m_leftPaddle = nullptr;
    Engine::Core::Object* m_rightPaddle = nullptr;
    glm::vec2 m_velocity { 0.f };
    float m_serveTimer = 0.f;
    float m_pendingServeDirection = 1.f;
    int m_serveIndex = 0;
};
