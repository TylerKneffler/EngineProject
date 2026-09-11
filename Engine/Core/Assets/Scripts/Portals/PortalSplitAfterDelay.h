#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"

#include <string>

// Demo-only portal controller. It waits while a named body remains split
// across this aperture, tears down the connection, then makes the two
// materialized mesh pieces fall independently.
class PortalSplitAfterDelay final : public Engine::Core::Script
{
public:
    PortalSplitAfterDelay();

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Split Demo")
    std::string traverserObjectName = "Split Demo Cube";

    PROPERTY(Inspector, EditAnywhere, Category = "Portal Split Demo")
    int destroyAfterFrames = 300; // five seconds at the fixed 60 Hz demo rate

    void Start() override;
    void Update() override;

private:
    int m_elapsedFrames = 0;
    bool m_destroyed = false;
};
