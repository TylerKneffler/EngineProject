#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

// Scene timing for the ordinary particle gallery. The emitter remains reusable;
// this script owns the repeated muzzle and tracer cues.
class ParticleVfxShowcaseDriver final : public Engine::Core::Script
{
public:
    ParticleVfxShowcaseDriver();

    PROPERTY(Inspector, EditAnywhere, Category = "Particle Demo")
    std::string muzzleObjectName = "Gun Flash | repeated burst";
    PROPERTY(Inspector, EditAnywhere, Category = "Particle Demo")
    std::string tracerObjectName = "Gun Flash | tracer";
    PROPERTY(Inspector, EditAnywhere, Category = "Particle Demo")
    float intervalSeconds = 0.65f;
    PROPERTY(Inspector, EditAnywhere, Category = "Particle Demo")
    int muzzleCount = 15;
    PROPERTY(Inspector, EditAnywhere, Category = "Particle Demo")
    int tracerCount = 3;

    void Start() override;
    void Update() override;

private:
    void Fire();
    float m_elapsed = 0.f;
};
