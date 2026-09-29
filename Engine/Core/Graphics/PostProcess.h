#pragma once
#include <cstdint>

namespace Engine::Graphics
{
struct PostProcessSettings
{
    float exposure = 1.0f;
    uint32_t toneMapping = 1;
};

void SetPostProcessSettings(const PostProcessSettings& settings);
PostProcessSettings GetPostProcessSettings();
}
