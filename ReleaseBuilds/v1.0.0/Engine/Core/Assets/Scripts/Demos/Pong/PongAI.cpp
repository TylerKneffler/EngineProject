#include "Scripts/Demos/Pong/PongAI.h"

#include "Scripts/Demos/Pong/PongGameManager.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

PongAI::PongAI()
{
    SetTypeName(COMPONENT_TYPE_NAME(PongAI));
    RegisterField("managerObjectName", managerObjectName);
    RegisterField("ballObjectName", ballObjectName);
    RegisterField("speed", speed);
    RegisterField("fieldHalfHeight", fieldHalfHeight);
}

namespace
{
struct PongAIRegistration
{
    PongAIRegistration()
    {
        Engine::Serialization::RegisterComponentType<PongAI>("PongAI");
    }
};
PongAIRegistration g_registration;
}

void PongAI::Start()
{
    ResolveObjects();
}

void PongAI::Update()
{
    if (!Owner || !Owner->GetScene())
        return;
    if (!m_manager || !m_ball)
        ResolveObjects();
    if (!m_manager || !m_ball || !m_manager->IsPlaying() ||
        !m_manager->IsRightPaddleAi())
        return;

    const float difference = m_ball->transform.position.y -
        Owner->transform.position.y;
    const float input = std::abs(difference) > 0.08f
        ? (difference > 0.f ? 1.f : -1.f) : 0.f;
    const float deltaTime = std::clamp(Owner->GetScene()->GetDeltaTime(),
        0.f, 0.05f);
    const float halfHeight = PaddleHalfHeight();
    Owner->transform.position.y = std::clamp(
        Owner->transform.position.y + input * speed * deltaTime,
        -fieldHalfHeight + halfHeight, fieldHalfHeight - halfHeight);
}

void PongAI::ResolveObjects()
{
    Engine::Core::Object* manager = FindObjectInSceneByName(managerObjectName);
    m_manager = manager ? manager->GetComponent<PongGameManager>() : nullptr;
    m_ball = FindObjectInSceneByName(ballObjectName);
}

float PongAI::PaddleHalfHeight() const
{
    if (!Owner)
        return 0.8f;
    const auto* collider =
        Owner->GetComponent<Engine::Components::PrimitiveObjectCollider>();
    return collider
        ? std::max(0.001f, collider->size.y * std::abs(Owner->transform.scale.y) * 0.5f)
        : 0.8f;
}
