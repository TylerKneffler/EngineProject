#pragma once

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/Lighting/ViewLighting.h"
#include "Core/Rendering/SceneRenderer.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Rendering
{
// View lighting that reads the scene: realtime light selection, the terrain
// sun, sky sun disc and the skybox/environment for one view. The scene-free
// pieces (clustered light lists, fog packing, skybox rays) are ViewLighting.
class SceneRendererLighting
{
public:
    using Scene = Engine::Scene::Scene;

    struct ViewLightingState
    {
        glm::vec3 terrainSunDirection{ 0.f, 1.f, 0.f };
        glm::vec3 terrainSunRadiance{ 0.f };
        int terrainSunIndex = -1;
        std::array<std::shared_ptr<Engine::Components::Texture>, 8> photometryTextures{};
    };

    static ViewLightingState ResolveViewLighting(
        SceneRenderer& renderer,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        const Engine::Model::LightData* frameLights,
        uint32_t frameLightCount,
        const std::array<std::string, 4>& cookiePaths,
        const std::array<std::string, 4>& iesPaths);

    static void PrepareSkyboxAndEnvironment(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& view,
        const glm::mat4& proj,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        bool shadowOnly,
        const glm::vec3& cameraPosition,
        const std::array<glm::vec4, 4>& atmosphereFog,
        const glm::vec3& sunDirection = glm::vec3(0.f),
        const glm::vec3& sunRadiance = glm::vec3(0.f));

    // Sun disc constants shared by the main and portal skybox draws:
    // direction (xyz, w = cosine of the disc radius) and radiance (rgb,
    // w = disc intensity); zero radiance disables the disc.
    static void ResolveSkySun(const SceneRenderer& renderer, const glm::vec3& sunDirection,
        const glm::vec3& sunRadiance, glm::vec4& direction, glm::vec4& radiance);
};
}
