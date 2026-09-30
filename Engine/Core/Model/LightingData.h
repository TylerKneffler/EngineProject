#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Engine::Model
{
    inline constexpr uint32_t MaxRealtimeLights = 64;

    enum class PortalShadowPolicy : uint32_t
    {
        ReuseMain = 0,
        Budgeted = 1,
        PerView = 2
    };

    struct BakedLightingSettings
    {
        uint32_t lightmapResolution = 256;
        float shadowBias = 0.002f;
        uint32_t dilationPasses = 4;
        bool accumulate = true;
    };

    // Project-wide budget and quality policy for realtime raster shadows.
    // Directional resolution is the total square atlas size. A single cascade
    // uses the whole atlas; two to four cascades use its 2x2 tile layout.
    struct RealtimeShadowSettings
    {
        bool enabled = true;
        uint32_t maximumShadowedLights = 1;
        uint32_t directionalResolution = 2048;
        uint32_t directionalCascadeCount = 4;
        float directionalDistance = 150.f;
        uint32_t pcfRadius = 1;
        float cascadeSplitLambda = 0.65f;
        float cascadeTransitionFraction = 0.1f;
        PortalShadowPolicy portalPolicy = PortalShadowPolicy::ReuseMain;
        uint32_t portalAtlasResolution = 1024;
        uint32_t portalCascadeCount = 2;
        uint32_t portalViewBudget = 2;
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
        bool realtimeShadows = true;
        float shadowDistanceScale = 1.f;
        uint32_t maximumShadowPcfRadius = 4;
    };

    struct DistanceLightingSettings
    {
        bool enabled = true;
        float maximumDistance = 1000.f;
        uint32_t maximumRealtimeLights = MaxRealtimeLights;
        bool clusteredLighting = true;
        uint32_t clusterTileSize = 64;
        uint32_t clusterDepthSlices = 16;
        uint32_t maximumLightsPerCluster = 32;
        std::vector<DistanceLightingBand> bands = {
            { "High", 250.f, MaxRealtimeLights, true, true, true, true,
                true, 1.f, 4u },
            { "Medium", 600.f, 8u, true, false, true, false,
                true, 0.65f, 1u },
            { "Low", 1000.f, 2u, false, false, false, false,
                false, 0.f, 0u }
        };
    };

    struct LightData
    {
        glm::vec4 positionRange{};
        glm::vec4 colorIntensity{};
        // x = legacy falloff, y = type (0 point, 1 directional, 2 spot),
        // z = shadow map index (-1 when
        // unshadowed), w = shadow strength. Object.hlsl currently ignores z/w
        // until the shadow sampling pass is connected.
        glm::vec4 params{ 0.f, 0.f, -1.f, 0.f };
        // xyz = direction emitted by a spot, w = cosine of its outer cone.
        glm::vec4 directionCone{ 0.f, 0.f, 1.f, -1.f };
        // x = cosine inner cone, y = physical attenuation flag.
        glm::vec4 attenuation{ 1.f, 0.f, 0.f, 0.f };
        // Local cookie axes and projection metadata. photometry.x/y are
        // one-based cookie/IES table indices; z is cookie scale.
        glm::vec4 cookieRight{ 1.f, 0.f, 0.f, 0.f };
        glm::vec4 cookieUp{ 0.f, 1.f, 0.f, 0.f };
        glm::vec4 photometry{};
    };

    struct RealtimeShadowSelection
    {
        std::string stableLightKey;
        glm::vec3 directionToLight{ 0.f, 1.f, 0.f };
        float depthBias = 0.0015f;
        float normalBias = 0.01f;
        float strength = 1.f;
        float resolutionScale = 1.f;
        float filterScale = 1.f;
        uint32_t lightIndex = 0;
        bool valid = false;
    };

    struct BakeResult
    {
        bool succeeded = false;
        uint32_t bakedLightCount = 0;
        uint32_t receiverCount = 0;
        std::string message;
    };
}
