#pragma once

#include "Core/Script.h"
#include <string>

// Scene choreography for the bundled fox soft-tissue demonstration.
class SoftTissueDemoDriver final : public Engine::Core::Script
{
public:
    SoftTissueDemoDriver();
    std::string foxObjectName = "Fox";
    std::string probeObjectName = "Fox Soft Tissue Probe";
    float repeatSeconds = 3.f;
    float probeHeight = 2.3f;
    float downwardSpeed = 3.f;

    void Start() override;
    void Update() override;
    unsigned GetReleaseCount() const { return m_releaseCount; }

private:
    bool ReleaseProbe();
    float m_elapsed = 0.f;
    unsigned m_releaseCount = 0;
};
