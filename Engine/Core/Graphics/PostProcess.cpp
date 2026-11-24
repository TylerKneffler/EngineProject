#include "pch.h"
#include "PostProcess.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Engine::Graphics
{
namespace { thread_local PostProcessSettings g_settings{}; }

void SetPostProcessSettings(const PostProcessSettings& settings)
{
    g_settings = settings;
}

PostProcessSettings GetPostProcessSettings()
{
    return g_settings;
}

PostProcessConstants MakeComposeConstants(const PostProcessSettings& settings,
    uint32_t bloomLevels, float sampleWeight)
{
    PostProcessConstants constants{};
    constants.params0[0] = settings.exposure;
    std::memcpy(&constants.params0[1], &settings.toneMapping, sizeof(uint32_t));
    constants.params0[2] = std::clamp(settings.bloomIntensity, 0.f, 1.f);
    constants.params0[3] = sampleWeight;
    // Outside accumulation (operator 4), w carries FXAA as a negative value:
    // -(edge threshold in thousandths + subpixel share * 0.999). PSMain decodes it.
    if (settings.fxaa && settings.toneMapping != 4u)
    {
        const float threshold = std::round(std::clamp(settings.fxaaEdgeThreshold, 0.03f, 0.35f) * 1000.f);
        constants.params0[3] = -(threshold + std::clamp(settings.fxaaSubpixel, 0.f, 1.f) * 0.999f);
    }
    for (int channel = 0; channel < 3; ++channel)
    {
        constants.shadowTint[channel] = settings.shadowTint[channel];
        constants.highlightTint[channel] = settings.highlightTint[channel];
    }
    constants.shadowTint[3] = settings.colorGrading ? 1.f : 0.f;
    constants.highlightTint[3] = std::clamp(settings.gradingBalance, -1.f, 1.f);
    constants.grade[0] = std::max(settings.gradingContrast, 0.01f);
    constants.grade[1] = std::max(settings.gradingSaturation, 0.f);
    constants.grade[2] = 1.f / static_cast<float>(std::max(bloomLevels, 1u));
    constants.grade[3] = settings.autoExposure ? 1.f : 0.f;
    constants.temporal[2] = settings.taa && settings.toneMapping != 4u
        ? std::clamp(settings.taaSharpen, 0.f, 1.f) : 0.f;
    return constants;
}

PostProcessConstants MakeTemporalConstants(const PostProcessSettings& settings, bool historyValid)
{
    PostProcessConstants constants{};
    float* rows[4] = { constants.params0, constants.shadowTint, constants.highlightTint, constants.grade };
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            rows[row][column] = settings.taaReprojection[row * 4 + column];
    constants.temporal[0] = std::clamp(settings.taaFeedback, 0.f, 0.98f);
    // temporal.y: 0 no usable history, else 1 + mode bits (the shader tests
    // < 0.5 for "no history"): 1 = ENGINE_TAA_HISTORY_DEBUG=1 (red where a
    // pixel has no history), 2 = fast settle off (taaFastSettle false, or
    // ENGINE_TAA_FAST_SETTLE=0 for A/B runs).
    static const bool historyDebug = [] {
        const char* value = std::getenv("ENGINE_TAA_HISTORY_DEBUG");
        return value && value[0] == '1'; }();
    static const bool settleOff = [] {
        const char* value = std::getenv("ENGINE_TAA_FAST_SETTLE");
        return value && value[0] == '0'; }();
    const float mode = (historyDebug ? 1.f : 0.f) + (settleOff || !settings.taaFastSettle ? 2.f : 0.f);
    // The fast-settle mode bits also apply when the history is unusable
    // (first frame, cut): mode / 8 stays below 0.5.
    constants.temporal[1] = historyValid && settings.taaHistoryValid ? 1.f + mode : 0.125f * mode;
    constants.temporal[2] = settings.taaJitter[0];
    constants.temporal[3] = settings.taaJitter[1];
    return constants;
}

PostProcessConstants MakeExposureConstants(const PostProcessSettings& settings,
    float deltaSeconds, bool history)
{
    PostProcessConstants constants{};
    constants.params0[0] = ExposureHistogramMinEv;
    constants.params0[1] = ExposureHistogramMaxEv - ExposureHistogramMinEv;
    constants.params0[2] = std::clamp(deltaSeconds, 0.f, 0.25f);
    constants.params0[3] = history ? 1.f : 0.f;
    const float low = std::clamp(settings.autoExposureLowPercent, 0.f, 0.99f);
    constants.shadowTint[0] = low;
    constants.shadowTint[1] = std::clamp(settings.autoExposureHighPercent, low + 0.01f, 1.f);
    constants.shadowTint[2] = std::max(settings.autoExposureSpeedUp, 0.f);
    constants.shadowTint[3] = std::max(settings.autoExposureSpeedDown, 0.f);
    const float minEv = std::clamp(settings.autoExposureMinEv, ExposureHistogramMinEv,
        ExposureHistogramMaxEv);
    constants.highlightTint[0] = minEv;
    constants.highlightTint[1] = std::clamp(settings.autoExposureMaxEv, minEv,
        ExposureHistogramMaxEv);
    constants.highlightTint[2] = 1.f / static_cast<float>(ExposureMapWidth);
    constants.highlightTint[3] = 1.f / static_cast<float>(ExposureMapHeight);
    constants.grade[0] = static_cast<float>(ExposureMapWidth);
    constants.grade[1] = static_cast<float>(ExposureMapHeight);
    constants.grade[2] = static_cast<float>(ExposureBins);
    // ExposureLuminancePS: ground weighting of the metering.
    constants.temporal[0] = std::clamp(settings.autoExposureGroundWeight, 0.f, 1.f);
    return constants;
}

PostProcessConstants MakeBloomPassConstants(uint32_t sourceWidth, uint32_t sourceHeight)
{
    PostProcessConstants constants{};
    constants.params0[0] = 1.f / static_cast<float>(std::max(sourceWidth, 1u));
    constants.params0[1] = 1.f / static_cast<float>(std::max(sourceHeight, 1u));
    return constants;
}

std::string LoadPostProcessShaderSource()
{
    const auto relative = std::filesystem::path("PostProcess") / "PostProcess.hlsl";
    std::filesystem::path path = std::filesystem::path("Engine") / "Shaders" / relative;
    if (!std::filesystem::is_regular_file(path))
        path = std::filesystem::path(ENGINE_SHADERS_PATH) / relative;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Post-process shader not found: " + path.string());
    std::ostringstream source;
    source << file.rdbuf();
    return source.str();
}
}
