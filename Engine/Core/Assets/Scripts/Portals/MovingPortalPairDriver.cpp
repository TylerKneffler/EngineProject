#include "Scripts/Portals/MovingPortalPairDriver.h"
#include "Scripts/Portals/MovingPortalPairDriver.h"

#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

MovingPortalPairDriver::MovingPortalPairDriver()
{
    SetTypeName(COMPONENT_TYPE_NAME(MovingPortalPairDriver));
    RegisterField("sourcePortalObjectName", sourcePortalObjectName);
    RegisterField("targetPortalObjectName", targetPortalObjectName);
    RegisterField("sourceTranslationAmplitude", sourceTranslationAmplitude);
    RegisterField("targetTranslationAmplitude", targetTranslationAmplitude);
    RegisterField("sourceRotationAmplitude", sourceRotationAmplitude);
    RegisterField("targetRotationAmplitude", targetRotationAmplitude);
    RegisterField("cyclesPerSecond", cyclesPerSecond);
    RegisterField("targetPhaseOffset", targetPhaseOffset);
    RegisterField("playAutomatically", playAutomatically);
}

namespace
{
struct MovingPortalPairDriverRegistration
{
    MovingPortalPairDriverRegistration()
    {
        Engine::Serialization::RegisterComponentType<MovingPortalPairDriver>(
            "MovingPortalPairDriver");
    }
};

MovingPortalPairDriverRegistration g_registration;

glm::vec3 TranslationWave(float phase)
{
    return {
        std::sin(phase),
        std::sin(phase * 0.73f + 0.45f),
        std::cos(phase * 0.89f + 0.2f)
    };
}

glm::vec3 RotationWave(float phase)
{
    return {
        std::sin(phase * 0.81f + 0.3f),
        std::sin(phase * 1.07f),
        std::cos(phase * 0.67f + 0.6f)
    };
}
}

Engine::Core::Object* MovingPortalPairDriver::ResolveSource() const
{
    return Owner ? Owner->FindObjectInSceneByName(sourcePortalObjectName) : nullptr;
}

Engine::Core::Object* MovingPortalPairDriver::ResolveTarget() const
{
    return Owner ? Owner->FindObjectInSceneByName(targetPortalObjectName) : nullptr;
}

bool MovingPortalPairDriver::EnsureBaseFrames()
{
    if (m_haveBaseFrames)
        return true;
    Engine::Core::Object* source = ResolveSource();
    Engine::Core::Object* target = ResolveTarget();
    if (!source || !target)
        return false;
    m_sourceBasePosition = source->transform.position;
    m_sourceBaseRotation = source->transform.rotation;
    m_targetBasePosition = target->transform.position;
    m_targetBaseRotation = target->transform.rotation;
    m_haveBaseFrames = true;
    return true;
}

void MovingPortalPairDriver::RecaptureBaseFrames()
{
    m_haveBaseFrames = false;
    EnsureBaseFrames();
}

void MovingPortalPairDriver::Start()
{
    m_elapsedSeconds = 0.f;
    m_lastFrame = std::chrono::steady_clock::now();
    RecaptureBaseFrames();
    ApplyAtTime(0.f);
}

void MovingPortalPairDriver::Update()
{
    const auto now = std::chrono::steady_clock::now();
    if (!playAutomatically)
    {
        m_lastFrame = now;
        return;
    }
    float delta = std::chrono::duration<float>(now - m_lastFrame).count();
    m_lastFrame = now;
    if (!(delta > 0.f && delta < 0.25f))
        delta = 1.f / 60.f;
    m_elapsedSeconds += delta;
    ApplyAtTime(m_elapsedSeconds);
}

void MovingPortalPairDriver::ApplyAtTime(float seconds)
{
    if (!EnsureBaseFrames())
        return;
    Engine::Core::Object* source = ResolveSource();
    Engine::Core::Object* target = ResolveTarget();
    if (!source || !target)
        return;

    const float phase = glm::two_pi<float>() *
        std::max(0.001f, cyclesPerSecond) * std::max(0.f, seconds);
    source->transform.position = m_sourceBasePosition +
        sourceTranslationAmplitude * TranslationWave(phase);
    source->transform.rotation = m_sourceBaseRotation +
        sourceRotationAmplitude * RotationWave(phase);
    target->transform.position = m_targetBasePosition +
        targetTranslationAmplitude * TranslationWave(phase + targetPhaseOffset);
    target->transform.rotation = m_targetBaseRotation +
        targetRotationAmplitude * RotationWave(phase + targetPhaseOffset);
    source->transform.MarkDirty();
    target->transform.MarkDirty();
}
