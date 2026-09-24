#pragma once

#include "Core/Script.h"
#include <glm/glm.hpp>

class SnakePlayer final : public Engine::Core::Script
{
public:
    SnakePlayer();

    void Start() override;
    void Update() override;
    void ResetDirection();
    glm::ivec2 ConsumeDirection(const glm::ivec2& currentDirection) const;

private:
    static bool IsKeyDown(int virtualKey);

    glm::ivec2 m_requestedDirection { 1, 0 };
};
