#include "pch.h"
#include "PostProcess.h"

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
