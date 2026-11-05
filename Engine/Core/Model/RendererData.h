#pragma once

#include <string>

namespace Engine::Model
{
enum class RenderScaleMode
{
    Native = 0,
    Quality75 = 1,
    Quality67 = 2,
    Quality50 = 3
};

struct RendererOption
{
    std::string name;
    bool available = false;
    std::string unavailableReason;
};

struct RenderQualitySettings
{
    RenderScaleMode scaleMode = RenderScaleMode::Native;
    float internalScale = 1.0f;
    bool dynamicInternalScale = false;
    bool temporalUpscaling = false;
    float targetFrameMs = 16.67f;
    float minimumScale = 0.5f;
    float maximumScale = 1.0f;

    static float ScaleForMode(RenderScaleMode mode)
    {
        switch (mode)
        {
        case RenderScaleMode::Quality75: return 0.75f;
        case RenderScaleMode::Quality67: return 0.67f;
        case RenderScaleMode::Quality50: return 0.50f;
        case RenderScaleMode::Native:
        default: return 1.0f;
        }
    }
};
}

