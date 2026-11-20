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

    // Physical sky: updates the atmosphere model/panorama for this camera,
    // multiplies the main directional light (the strongest) by the
    // atmosphere's transmittance toward the sun, and records the view's
    // atmosphere terms on the renderer. Call right after collecting lights.
    static void ApplyPhysicalAtmosphere(
        SceneRenderer& renderer,
        Engine::Model::LightData* frameLights,
        uint32_t frameLightCount,
        const glm::vec3& cameraPosition);

    // Volumetric clouds: ray-marches this view's cloud layer at half
    // resolution in a compute pass (call before PrepareSkyboxAndEnvironment,
    // outside the sky draw), which then composites it over the sky.
    // Screen-space ambient occlusion for the next frame, from this view's
    // depth so far (call after the main draws, outside any depth-only pass).
    // Rebinds structured slot 47 to the result for the rest of the frame.
    static void ComputeAmbientOcclusion(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& view,
        const glm::mat4& proj,
        uint32_t viewportWidth,
        uint32_t viewportHeight);
    // Builds the GPU occlusion pyramid from the depth the AO pass just copied
    // (call right after it, for the main view). camera identifies the view.
    static void BuildDepthPyramid(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context,
        const void* camera,
        const glm::mat4& view,
        const glm::mat4& viewProjection,
        const glm::mat4& proj,
        uint32_t viewportWidth,
        uint32_t viewportHeight);
    // The pyramid the previous frame built for this camera, when the camera
    // has moved and turned little enough since for its occlusion to hold;
    // else null. ENGINE_GPU_OCCLUSION=0 disables it.
    static const Engine::Graphics::IGraphicsBuffer* DepthPyramidForCulling(
        SceneRenderer& renderer,
        const void* camera,
        const glm::mat4& view,
        uint32_t viewportWidth,
        uint32_t viewportHeight);
    // The buffer draws read at structured slot 47.
    static const Engine::Graphics::IGraphicsBuffer* AmbientOcclusionBuffer(
        SceneRenderer& renderer);
    // The main view's depth as a structured float buffer that pixel shaders
    // may read (soft particles), or null. Reuses the AO pass's copy when it
    // ran for this view, otherwise copies now. Call outside depth-only passes,
    // after ComputeAmbientOcclusion for the view.
    static const Engine::Graphics::IGraphicsBuffer* SceneDepthForPixelShaders(
        SceneRenderer& renderer, Engine::Graphics::IGraphicsContext* context);

    static void MarchVolumetricClouds(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& view,
        const glm::mat4& proj,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        bool shadowOnly,
        const glm::vec3& cameraPosition,
        const glm::vec3& sunDirection,
        const glm::vec3& sunRadiance);

    // Sun disc constants shared by the main and portal skybox draws:
    // direction (xyz, w = cosine of the disc radius) and radiance (rgb,
    // w = disc intensity); zero radiance disables the disc.
    static void ResolveSkySun(const SceneRenderer& renderer, const glm::vec3& sunDirection,
        const glm::vec3& sunRadiance, glm::vec4& direction, glm::vec4& radiance);
};
}
