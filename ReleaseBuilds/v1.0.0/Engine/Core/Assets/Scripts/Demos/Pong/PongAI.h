#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

class PongGameManager;

class PongAI final : public Engine::Core::Script
{
public:
    PongAI();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | AI")
    std::string managerObjectName = "Pong Game";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | AI")
    std::string ballObjectName = "Ball";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | AI", ClampMin = "0.1")
    float speed = 6.2f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfHeight = 4.35f;

    void Start() override;
    void Update() override;

private:
    void ResolveObjects();
    float PaddleHalfHeight() const;

    PongGameManager* m_manager = nullptr;
    Engine::Core::Object* m_ball = nullptr;
};
