#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace Engine::Rendering::Lighting
{
    inline constexpr uint32_t MaxRealtimeLights = 64;

    struct BakedLightingSettings
    {
        // Kept per-project so all scenes bake at a consistent texel density.
        uint32_t lightmapResolution = 256;
        float shadowBias = 0.002f;
        uint32_t dilationPasses = 4;
        bool accumulate = true;
    };

    // Shared CPU/GPU layout for point and global directional lights.
    struct LightData
    {
        // Point: xyz position, w range. Global: xyz direction toward the light.
        glm::vec4 positionRange{};
        glm::vec4 colorIntensity{};
        glm::vec4 params{}; // falloff, type (0 point, 1 global), reserved
    };

    struct BakeResult
    {
        bool succeeded = false;
        uint32_t bakedLightCount = 0;
        uint32_t receiverCount = 0;
        std::string message;
    };
}
