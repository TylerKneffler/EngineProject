#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace Engine::Graphics
{
struct PostProcessSettings
{
    float exposure = 1.0f;
    uint32_t toneMapping = 1;
    // Fraction of the blurred bloom chain mixed into the HDR image. Zero
    // skips the bloom passes entirely.
    float bloomIntensity = 0.f;
    bool colorGrading = false;
    float shadowTint[3] = { 1.f, 1.f, 1.f };
    float highlightTint[3] = { 1.f, 1.f, 1.f };
    float gradingBalance = 0.f;
    float gradingContrast = 1.f;
    float gradingSaturation = 1.f;
};

void SetPostProcessSettings(const PostProcessSettings& settings);
PostProcessSettings GetPostProcessSettings();

// Every post-process pass shares this 64-byte constant block (a DX cbuffer,
// DX12 root constants, or a Vulkan push-constant range). Layout matches
// Shaders/PostProcess/PostProcess.hlsl.
struct PostProcessConstants
{
    // Compose: exposure, operator (uint bits), bloom intensity, sample weight.
    // Bloom passes: source texel size xy.
    float params0[4] = {};
    float shadowTint[4] = {}; // rgb, w = grading enabled
    float highlightTint[4] = {}; // rgb, w = shadow/highlight balance
    float grade[4] = {}; // contrast, saturation, 1 / bloom levels, unused
};
static_assert(sizeof(PostProcessConstants) == 64);

PostProcessConstants MakeComposeConstants(const PostProcessSettings& settings,
    uint32_t bloomLevels, float sampleWeight = 1.f);
PostProcessConstants MakeBloomPassConstants(uint32_t sourceWidth, uint32_t sourceHeight);

// Bloom renders into a half-resolution mip chain. Levels stop before the
// smallest mip gets narrower than about eight pixels.
constexpr uint32_t MaxBloomLevels = 6;
inline uint32_t BloomLevelCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 0;
    uint32_t side = std::min(width, height) / 2u;
    while (levels < MaxBloomLevels && side >= 8u)
    {
        ++levels;
        side /= 2u;
    }
    return std::max(levels, 1u);
}
inline uint32_t BloomLevelExtent(uint32_t fullExtent, uint32_t level)
{
    return std::max(1u, (fullExtent / 2u) >> level);
}

// HLSL source of Shaders/PostProcess/PostProcess.hlsl for backends that
// compile at runtime. Throws when the file cannot be found.
std::string LoadPostProcessShaderSource();
}
