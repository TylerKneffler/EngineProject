#pragma once

#include "Core/Compoonents/Path/SplineFollower.h"

// Compatibility component for cameras and video export. The path playback
// implementation is general-purpose and shared with ordinary scene objects.
class CameraTrack final : public SplineFollower
{
public:
    CameraTrack();
};
