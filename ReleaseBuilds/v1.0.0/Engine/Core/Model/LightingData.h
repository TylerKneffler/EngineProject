#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

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

    // Project-wide lighting work selected from the nearest visible point of
    // each object. Bands are evaluated in order and may be freely renamed or
    // reconfigured by a project.
    struct DistanceLightingBand
    {
        std::string name = "Quality";
        float endDistance = 1000.f;
        uint32_t maxRealtimeLights = MaxRealtimeLights;
        bool normalMapping = true;
        bool parallaxMapping = true;
        bool environmentDiffuse = true;
        bool reflections = true;
    };

    struct DistanceLightingSettings
    {
        bool enabled = true;
        float maximumDistance = 1000.f;
        std::vector<DistanceLightingBand> bands = {
            { "High", 250.f, MaxRealtimeLights, true, true, true, true },
            { "Medium", 600.f, 8u, true, false, true, false },
            { "Low", 1000.f, 2u, false, false, false, false }
        };
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
