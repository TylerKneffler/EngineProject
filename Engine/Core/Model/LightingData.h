#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace Engine::Model
{
    inline constexpr uint32_t MaxRealtimeLights = 64;

    struct BakedLightingSettings
    {
        uint32_t lightmapResolution = 256;
        float shadowBias = 0.002f;
        uint32_t dilationPasses = 4;
        bool accumulate = true;
    };

    struct LightData
    {
        glm::vec4 positionRange{};
        glm::vec4 colorIntensity{};
        glm::vec4 params{};
    };

    struct BakeResult
    {
        bool succeeded = false;
        uint32_t bakedLightCount = 0;
        uint32_t receiverCount = 0;
        std::string message;
    };
}
