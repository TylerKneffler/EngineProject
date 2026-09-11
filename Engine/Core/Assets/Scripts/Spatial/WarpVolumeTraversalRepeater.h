#pragma once

#include "Core/PropertyMacros.h"
#include "Core/Script.h"
#include <string>

// Reverses a rigid body's velocity after it clears either longitudinal end of
// a box warp volume. This keeps bidirectional metric traversal observable in
// test scenes without embedding demo-specific motion in SpatialManipulator.
class WarpVolumeTraversalRepeater final : public Engine::Core::Script
{
public:
    WarpVolumeTraversalRepeater();

    PROPERTY(Inspector, EditAnywhere, Category = "Warp Volume Repeater")
    std::string volumeObjectName = "Smooth Shrinking Tunnel Warp";

    PROPERTY(Inspector, EditAnywhere, Category = "Warp Volume Repeater", ClampMin = "0")
    float turnaroundClearance = 1.f;

    void Update() override;
};
