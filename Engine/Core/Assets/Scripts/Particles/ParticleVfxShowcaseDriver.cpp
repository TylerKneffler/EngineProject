#include "Scripts/Particles/ParticleVfxShowcaseDriver.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

ParticleVfxShowcaseDriver::ParticleVfxShowcaseDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(ParticleVfxShowcaseDriver));
    RegisterField("muzzleObjectName", muzzleObjectName);
    RegisterField("tracerObjectName", tracerObjectName);
    RegisterField("intervalSeconds", intervalSeconds);
    RegisterField("muzzleCount", muzzleCount);
    RegisterField("tracerCount", tracerCount);
}

namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<ParticleVfxShowcaseDriver>(
            "ParticleVfxShowcaseDriver");
    }
} registration;
}

void ParticleVfxShowcaseDriver::Start()
{
    m_elapsed = 0.f;
    if (Owner)
        for (const auto& objectName : {muzzleObjectName, tracerObjectName})
            if (auto* object = Owner->FindObjectInSceneByName(objectName))
                if (auto* emitter = object->GetComponent<
                        Engine::Components::ParticleEmitter>())
                {
                    emitter->burstOnStart = 0;
                    emitter->loop = false;
                    emitter->Restart();
                }
    Fire();
}

void ParticleVfxShowcaseDriver::Update()
{
    if (!Owner || !Owner->GetScene()) return;
    const float delta = Owner->GetScene()->GetDeltaTime();
    if (!std::isfinite(delta) || delta <= 0.f) return;
    m_elapsed += std::min(delta, 0.25f);
    const float interval = std::clamp(intervalSeconds, 0.05f, 10.f);
    if (m_elapsed >= interval)
    {
        m_elapsed = std::fmod(m_elapsed, interval);
        Fire();
    }
}

void ParticleVfxShowcaseDriver::Fire()
{
    if (!Owner) return;
    const auto burst = [this](const std::string& objectName, int count)
    {
        if (auto* object = Owner->FindObjectInSceneByName(objectName))
            if (auto* emitter = object->GetComponent<
                    Engine::Components::ParticleEmitter>())
                emitter->Burst(std::clamp(count, 0, 1000));
    };
    burst(muzzleObjectName, muzzleCount);
    burst(tracerObjectName, tracerCount);
}
