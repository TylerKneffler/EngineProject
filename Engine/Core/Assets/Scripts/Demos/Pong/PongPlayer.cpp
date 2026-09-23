#include "Scripts/Demos/Pong/PongPlayer.h"

#include "Scripts/Demos/Pong/PongGameManager.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

PongPlayer::PongPlayer()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongPlayer));
    RegisterField("managerObjectName", managerObjectName);
    RegisterField("rightPlayer", rightPlayer);
    RegisterField("upKey", upKey);
    RegisterField("downKey", downKey);
    RegisterField("speed", speed);
    RegisterField("fieldHalfHeight", fieldHalfHeight);
}

namespace
{
struct PongPlayerRegistration
{
    PongPlayerRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongPlayer>("PongPlayer");
    }
};
PongPlayerRegistration g_registration;
}

void PongPlayer::Start()
{
    ResolveManager();
}

void PongPlayer::Update()
{
    if (!Owner || !Owner->GetScene())
        return;
    if (!m_manager)
        ResolveManager();
    const bool playing = m_manager && m_manager->IsPlaying();
    if (playing && !m_wasPlaying)
        Owner->transform.position.y = 0.f;
    m_wasPlaying = playing;
    if (!playing || (rightPlayer && m_manager->IsRightPaddleAi()))
        return;

    const float input = static_cast<float>(IsKeyDown(upKey)) -
        static_cast<float>(IsKeyDown(downKey));
    const float deltaTime = std::clamp(Owner->GetScene()->GetDeltaTime(),
        0.f, 0.05f);
    const float halfHeight = PaddleHalfHeight();
    Owner->transform.position.y = std::clamp(
        Owner->transform.position.y + input * speed * deltaTime,
        -fieldHalfHeight + halfHeight, fieldHalfHeight - halfHeight);
}

void PongPlayer::ResolveManager()
{
    Engine::Core::Object* manager = FindObjectInSceneByName(managerObjectName);
    m_manager = manager ? manager->GetComponent<PongGameManager>() : nullptr;
}

float PongPlayer::PaddleHalfHeight() const
{
    if (!Owner)
        return 0.8f;
    const auto* collider =
        Owner->GetComponent<Engine::Components::PrimitiveObjectCollider>();
    return collider
        ? std::max(0.001f, collider->size.y * std::abs(Owner->transform.scale.y) * 0.5f)
        : 0.8f;
}

bool PongPlayer::IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}
