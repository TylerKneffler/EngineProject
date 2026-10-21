#include "Scripts/Particles/FluidTestSceneDriver.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

FluidTestSceneDriver::FluidTestSceneDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(FluidTestSceneDriver));
    RegisterField("fluidObjectName", fluidObjectName);
    RegisterField("paddleObjectName", paddleObjectName);
    RegisterField("restartSeconds", restartSeconds);
    RegisterField("paddleAmplitude", paddleAmplitude);
    RegisterField("paddleCyclesPerSecond", paddleCyclesPerSecond);
    RegisterField("poolSpawnPosition", poolSpawnPosition);
    RegisterField("poolSpawnHalfExtents", poolSpawnHalfExtents);
}

namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<FluidTestSceneDriver>(
            "FluidTestSceneDriver");
    }
} registration;
}

void FluidTestSceneDriver::Start()
{
    m_elapsed = 0.f;
    m_sinceRestart = 0.f;
    if (!Owner) return;
    if (auto* paddle = Owner->FindObjectInSceneByName(paddleObjectName))
        m_paddleOrigin = paddle->transform.position;
    if (auto* water = Owner->FindObjectInSceneByName(fluidObjectName))
        if (auto* emitter = water->GetComponent<
                Engine::Components::ParticleEmitter>())
        {
            if (m_poolSpawnPending)
            {
                water->transform.position = m_streamOrigin;
                emitter->boxExtents = m_streamHalfExtents;
            }
            m_streamOrigin = water->transform.position;
            m_streamHalfExtents = emitter->boxExtents;
            water->transform.position = poolSpawnPosition;
            emitter->boxExtents = poolSpawnHalfExtents;
            emitter->Restart();
            m_poolSpawnPending = true;
        }
}

void FluidTestSceneDriver::Update()
{
    if (!Owner || !Owner->GetScene()) return;
    const float delta = Owner->GetScene()->GetDeltaTime();
    if (!std::isfinite(delta) || delta <= 0.f) return;
    m_elapsed += std::min(delta, 0.25f);
    m_sinceRestart += std::min(delta, 0.25f);
    if (m_poolSpawnPending)
        if (auto* water = Owner->FindObjectInSceneByName(fluidObjectName))
            if (auto* emitter = water->GetComponent<
                    Engine::Components::ParticleEmitter>())
                if (emitter->GetSimulationTicks() > 0)
                {
                    water->transform.position = m_streamOrigin;
                    emitter->boxExtents = m_streamHalfExtents;
                    m_poolSpawnPending = false;
                }
    constexpr float Tau = 6.28318530717958647692f;
    const float angularSpeed = Tau *
        std::clamp(paddleCyclesPerSecond, 0.f, 2.f);
    const float amplitude = std::clamp(paddleAmplitude, 0.f, 1.f);
    if (auto* paddle = Owner->FindObjectInSceneByName(paddleObjectName))
    {
        const glm::vec3 position = m_paddleOrigin + glm::vec3(
            amplitude * std::sin(angularSpeed * m_elapsed), 0.f, 0.f);
        if (auto* body = paddle->GetComponent<Engine::Components::RigidBody>())
        {
            body->SetWorldPosition(position);
            body->SetLinearVelocity({amplitude * angularSpeed *
                std::cos(angularSpeed * m_elapsed), 0.f, 0.f});
        }
        else
            paddle->transform.position = position;
    }
    if (m_sinceRestart >= std::max(1.f, restartSeconds))
    {
        m_sinceRestart = 0.f;
        if (auto* object = Owner->FindObjectInSceneByName(fluidObjectName))
            if (auto* emitter = object->GetComponent<
                    Engine::Components::ParticleEmitter>())
            {
                object->transform.position = poolSpawnPosition;
                emitter->boxExtents = poolSpawnHalfExtents;
                emitter->Restart();
                m_poolSpawnPending = true;
            }
    }
}
