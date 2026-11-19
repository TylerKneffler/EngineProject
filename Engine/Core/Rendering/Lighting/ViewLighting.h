#pragma once

#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Model/LightingData.h"
#include "Core/Model/SceneSettings.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Components { class Texture; }

namespace Engine::Rendering
{
// Per-view lighting work that needs no scene: clustered light lists, IES /
// cookie texture binding, atmosphere fog packing and skybox ray constants.
// Any renderer drawing object shaders (Object.hlsl, Skybox.hlsl) can use it.
class ViewLighting
{
public:
    struct ClusterLightingConfig
    {
        uint32_t clusterTilesX = 0u;
        uint32_t clusterTilesY = 0u;
        uint32_t clusterDepthSlices = 0u;
        uint32_t maximumLightsPerCluster = 0u;
        uint32_t clusterCount = 0u;
        uint32_t indexCount = 0u;
        bool enabled = false;
        bool directionalOnly = false;
    };

    static ClusterLightingConfig PrepareClusteredLighting(
        Engine::Graphics::IGraphicsProvider* graphicsProvider,
        const glm::mat4& view,
        const glm::mat4& proj,
        float cameraNear, float cameraFar,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        const Engine::Model::LightData* frameLights,
        uint32_t frameLightCount,
        const Engine::Model::DistanceLightingSettings& distanceLightingSettings,
        bool shadowOnly,
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer>& clusterHeaderBuffer,
        void*& clusterHeaderMapped,
        uint32_t& clusterHeaderCapacity,
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer>& clusterLightIndexBuffer,
        void*& clusterLightIndexMapped,
        uint32_t& clusterLightIndexCapacity,
        std::vector<glm::uvec2>& clusterHeaderScratch,
        std::vector<uint32_t>& clusterLightIndexScratch,
        std::vector<std::unique_ptr<Engine::Graphics::IGraphicsBuffer>>& retiredRenderBuffers,
        std::map<std::pair<const void*, const void*>, std::array<uint32_t, 5>>& directionalClusterLayouts,
        double* preparationMs = nullptr,
        uint64_t* uploadBytes = nullptr,
        uint64_t* cacheHits = nullptr);

    static void BindPhotometryTextures(
        Engine::Graphics::IGraphicsContext* context,
        const std::array<std::shared_ptr<Engine::Components::Texture>, 8>& photometryTextures);

    // Packs SceneSettings::atmosphereFog for the object shaders
    // (AtmosphereFog.hlsli: fog0..fog3). All zero when fog is disabled.
    static std::array<glm::vec4, 4> PackAtmosphereFog(
        const Engine::Model::AtmosphereFogSettings& fog,
        const glm::vec3& sunDirection,
        const glm::vec3& sunRadiance,
        bool hasSun);

    // Physical aerial perspective for AtmosphereFog.hlsli (same fog0..fog3
    // slots; fog0.w >= 100 selects it). sunIlluminance is the sun reaching
    // the camera (after the atmosphere); multipleScattering is Psi_ms times
    // the sun illuminance. Light shafts use the height-fog shaft settings.
    static std::array<glm::vec4, 4> PackPhysicalAerialPerspective(
        const Engine::Model::PhysicalSkySettings& sky,
        const Engine::Model::AtmosphereFogSettings& shafts,
        const glm::vec3& toSun,
        const glm::vec3& sunIlluminance,
        const glm::vec3& multipleScattering);

    // Skybox.hlsl constants. 128 bytes: the Vulkan push-constant limit.
    // The world view ray is affine in NDC, so rayX * ndc.x + rayY * ndc.y +
    // rayOrigin replaces the inverse view-projection; the w components carry
    // the sky fog density at the camera, height falloff and ray length.
    struct SkyboxConstants
    {
        glm::vec4 rayX{0.f};
        glm::vec4 rayY{0.f};
        glm::vec4 rayOrigin{0.f};
        glm::vec4 displayParams{0.f}; // 2D, exposure, rotation, sun glow
        glm::vec4 sunDirection{0.f};  // toward the sun, w = cos(disc radius)
        glm::vec4 sunRadiance{0.f};   // rgb, w = disc intensity
        glm::vec4 fogInscatter{0.f};  // ambient inscatter rgb, w = anisotropy
        glm::vec4 fogSunGlow{0.f};    // fog sun glow rgb
    };
    static_assert(sizeof(SkyboxConstants) == 128);

    // Clouds.hlsl constants (128 bytes). The ray basis is the same as the
    // sky's; see the shader for the packing of the w components.
    struct CloudConstants
    {
        glm::vec4 rayX{0.f};         // w: time (s)
        glm::vec4 rayY{0.f};         // w: base height
        glm::vec4 rayOrigin{0.f};    // w: top height
        glm::vec4 camera{0.f};       // world position, w: coverage
        glm::vec4 sunDirection{0.f}; // toward the sun, w: extinction per metre
        glm::vec4 sunRadiance{0.f};  // rgb, w: max distance
        glm::vec4 ambient{0.f};      // sky light rgb, w: shape scale (m)
        glm::vec4 shape{0.f};        // x/z wind offset (m), y: detail, w: steps
    };
    static_assert(sizeof(CloudConstants) == 128);

    // Sets the ray basis (xyz only) for a direction-only view (no translation).
    static void SetSkyboxRays(SkyboxConstants& constants, const glm::mat4& proj,
        const glm::mat4& directionOnlyView);
    // Sky fog terms (ray-basis w, fogInscatter, fogSunGlow) for a camera at
    // cameraHeight, from PackAtmosphereFog's output. Leaves the constants
    // unfogged when the packed fog is off or skyDistance <= 0.
    static void SetSkyboxFog(SkyboxConstants& constants,
        const std::array<glm::vec4, 4>& atmosphereFog, float skyDistance,
        float cameraHeight);
};
}
