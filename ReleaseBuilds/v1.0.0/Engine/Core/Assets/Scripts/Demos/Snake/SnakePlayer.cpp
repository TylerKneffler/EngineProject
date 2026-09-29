#include "Scripts/Demos/Snake/SnakePlayer.h"

#include "Core/Serialization/SceneSerializer.h"

SnakePlayer::SnakePlayer()
{
    SetTypeName(COMPONENT_TYPE_NAME(SnakePlayer));
}

namespace
{
struct SnakePlayerRegistration
{
    SnakePlayerRegistration()
    {
        Engine::Serialization::RegisterComponentType<SnakePlayer>("SnakePlayer");
    }
};
SnakePlayerRegistration g_registration;
}

void SnakePlayer::Start()
{
    ResetDirection();
}

void SnakePlayer::Update()
{
    if (IsKeyDown(VK_UP) || IsKeyDown('W'))
        m_requestedDirection = { 0, 1 };
    else if (IsKeyDown(VK_DOWN) || IsKeyDown('S'))
        m_requestedDirection = { 0, -1 };
    else if (IsKeyDown(VK_LEFT) || IsKeyDown('A'))
        m_requestedDirection = { -1, 0 };
    else if (IsKeyDown(VK_RIGHT) || IsKeyDown('D'))
        m_requestedDirection = { 1, 0 };
}

void SnakePlayer::ResetDirection()
{
    m_requestedDirection = { 1, 0 };
}

glm::ivec2 SnakePlayer::ConsumeDirection(
    const glm::ivec2& currentDirection) const
{
    if (m_requestedDirection + currentDirection == glm::ivec2(0))
        return currentDirection;
    return m_requestedDirection;
}

bool SnakePlayer::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}
