#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include <chrono>
#include <glm/glm.hpp>
#include <string>

// Animates a linked portal pair from stable authored poses. ApplyAtTime is
// public so deterministic tests and replay systems can sample the same motion
// without relying on wall-clock time.
class MovingPortalPairDriver final : public Engine::Core::Script
{
public:
    MovingPortalPairDriver();

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    std::string sourcePortalObjectName = "Moving Source Portal";

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    std::string targetPortalObjectName = "Moving Target Portal";

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    glm::vec3 sourceTranslationAmplitude { 0.8f, 0.25f, 0.2f };

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    glm::vec3 targetTranslationAmplitude { 0.6f, 0.35f, 0.25f };

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    glm::vec3 sourceRotationAmplitude { 0.12f, 0.22f, 0.08f };

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    glm::vec3 targetRotationAmplitude { 0.16f, 0.28f, 0.1f };

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair", ClampMin = "0.001")
    float cyclesPerSecond = 0.12f;

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    float targetPhaseOffset = 1.5707963268f;

    PROPERTY(Inspector, EditAnywhere, Category = "Moving Portal Pair")
    bool playAutomatically = true;

    void Start() override;
    void Update() override;

    void ApplyAtTime(float seconds);
    void RecaptureBaseFrames();

private:
    Engine::Core::Object* ResolveSource() const;
    Engine::Core::Object* ResolveTarget() const;
    bool EnsureBaseFrames();

    glm::vec3 m_sourceBasePosition { 0.f };
    glm::vec3 m_sourceBaseRotation { 0.f };
    glm::vec3 m_targetBasePosition { 0.f };
    glm::vec3 m_targetBaseRotation { 0.f };
    float m_elapsedSeconds = 0.f;
    bool m_haveBaseFrames = false;
    std::chrono::steady_clock::time_point m_lastFrame;
};
