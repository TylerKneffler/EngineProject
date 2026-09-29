#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

class PongGameManager;

class PongPlayer final : public Engine::Core::Script
{
public:
    PongPlayer();

    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Player")
    std::string managerObjectName = "Pong Game";
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Player")
    bool rightPlayer = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Player")
    int upKey = 'W';
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Player")
    int downKey = 'S';
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Player", ClampMin = "0.1")
    float speed = 7.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "Pong | Court", ClampMin = "1")
    float fieldHalfHeight = 4.35f;

    void Start() override;
    void Update() override;

private:
    void ResolveManager();
    float PaddleHalfHeight() const;
    static bool IsKeyDown(int virtualKey);

    PongGameManager* m_manager = nullptr;
    bool m_wasPlaying = false;
};
