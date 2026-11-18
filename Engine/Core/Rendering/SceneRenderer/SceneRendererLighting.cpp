#include "Core/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Rendering/RenderUtilities.h"

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Model/LightingData.h"
#include "Core/Rendering/SceneRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
SceneRendererLighting::ViewLightingState SceneRendererLighting::ResolveViewLighting(
    SceneRenderer& renderer,
    const glm::vec3& cameraPosition,
    bool shadowOnly,
    const Engine::Model::LightData* frameLights,
    uint32_t frameLightCount,
    const std::array<std::string, 4>& cookiePaths,
    const std::array<std::string, 4>& iesPaths)
{
    ViewLightingState state{};
    if (shadowOnly)
        return state;

    for (uint32_t index = 0; index < frameLightCount; ++index)
    {
        const auto& light = frameLights[index];
        if (std::abs(light.params.y - 1.f) >= 0.25f)
            continue;

        const glm::vec3 radiance = glm::vec3(light.colorIntensity) * light.colorIntensity.w;
        if (state.terrainSunIndex >= 0 &&
            glm::length(radiance) <= glm::length(state.terrainSunRadiance))
        {
            continue;
        }

        state.terrainSunDirection = glm::normalize(glm::vec3(light.positionRange));
        state.terrainSunRadiance = radiance;
        state.terrainSunIndex = static_cast<int>(index);
    }

    for (size_t index = 0; index < cookiePaths.size(); ++index)
    {
        if (!cookiePaths[index].empty())
        {
            state.photometryTextures[index] = Engine::Components::Texture::Acquire(
                cookiePaths[index], false);
            state.photometryTextures[index]->Prepare(renderer.m_graphicsProvider);
        }
        if (!iesPaths[index].empty())
        {
            state.photometryTextures[index + 4u] = Engine::Components::Texture::Acquire(
                iesPaths[index], false);
            state.photometryTextures[index + 4u]->Prepare(renderer.m_graphicsProvider);
        }
    }

    for (uint32_t index = 0; index < state.photometryTextures.size(); ++index)
    {
        if (state.photometryTextures[index])
            state.photometryTextures[index]->RequestMipFootprint(std::numeric_limits<float>::infinity());
    }

    (void)cameraPosition;
    return state;
}

void SceneRendererLighting::ResolveSkySun(const SceneRenderer& renderer,
    const glm::vec3& sunDirection, const glm::vec3& sunRadiance,
    glm::vec4& direction, glm::vec4& radiance)
{
    const Scene& scene = renderer.GetScene();
    const auto& settings = scene.settings;
    direction = glm::vec4(0.f);
    radiance = glm::vec4(0.f);
    if (!settings.sunDiscEnabled || scene.IsMode2D() ||
        glm::dot(sunDirection, sunDirection) < 1e-8f ||
        glm::dot(sunRadiance, sunRadiance) <= 0.f)
        return;
    const float radius = glm::radians(std::clamp(
        settings.sunDiscAngularDiameter, 0.05f, 20.f) * 0.5f);
    direction = glm::vec4(glm::normalize(sunDirection), std::cos(radius));
    radiance = glm::vec4(sunRadiance, std::max(settings.sunDiscIntensity, 0.f));
}

void SceneRendererLighting::PrepareSkyboxAndEnvironment(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const glm::mat4& view,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool shadowOnly,
    const glm::vec3& cameraPosition,
    const std::array<glm::vec4, 4>& atmosphereFog,
    const glm::vec3& sunDirection,
    const glm::vec3& sunRadiance)
{
    Scene& scene = renderer.GetScene();
    if (!shadowOnly)
        ResolveSkySun(renderer, sunDirection, sunRadiance,
            renderer.m_skySunDirection, renderer.m_skySunRadiance);
    const Engine::Components::Texture* skybox = shadowOnly
        ? nullptr
        : renderer.ResolveSkyboxTexture();
    if (!shadowOnly)
        renderer.UpdateEnvironmentLighting(skybox);

    if (!shadowOnly && skybox && skybox->GetGraphicsTexture() && renderer.m_skyboxPipeline)
    {
        // A panorama repeat spans the full environment, beyond the camera FOV.
        Engine::Components::Texture::RequestGraphicsFootprint(
            skybox->GetGraphicsTexture(),
            4.f * float(std::max(viewportWidth, viewportHeight)));

        ViewLighting::SkyboxConstants skyboxData{};
        skyboxData.sunDirection = renderer.m_skySunDirection;
        skyboxData.sunRadiance = renderer.m_skySunRadiance;
        // A skybox represents direction only. Excluding camera translation
        // prevents cancellation in the far - near ray math when the editor
        // camera is far from the world origin.
        ViewLighting::SetSkyboxRays(skyboxData, proj, glm::mat4(glm::mat3(view)));
        if (atmosphereFog[0].w > 0.5f &&
            scene.settings.atmosphereFog.skyDistance > 0.f &&
            !scene.IsMode2D())
        {
            skyboxData.rayX.w = atmosphereFog[0].x * std::exp(
                std::clamp(-atmosphereFog[0].y *
                    (cameraPosition.y - atmosphereFog[0].z), -60.f, 60.f));
            skyboxData.rayY.w = atmosphereFog[0].y;
            skyboxData.rayOrigin.w = scene.settings.atmosphereFog.skyDistance;
            skyboxData.fogInscatter = { glm::vec3(atmosphereFog[1]), atmosphereFog[2].w };
            skyboxData.fogSunGlow = { glm::vec3(atmosphereFog[3]), 0.f };
            // The fog glow needs the sun even when its disc is not drawn
            // (zero radiance keeps the disc and its halo off).
            if (glm::dot(glm::vec3(skyboxData.sunDirection), glm::vec3(skyboxData.sunDirection)) < 1e-8f)
                skyboxData.sunDirection = { glm::vec3(atmosphereFog[2]), skyboxData.sunDirection.w };
        }
        skyboxData.displayParams = {
            scene.IsMode2D() ? 1.f : 0.f,
            std::exp2(scene.settings.hdriExposure),
            glm::radians(scene.settings.hdriRotation),
            std::max(scene.settings.sunGlowIntensity, 0.f)};

        std::memcpy(renderer.m_skyboxCBMapped, &skyboxData, sizeof(skyboxData));
        context->SetPipeline(renderer.m_skyboxPipeline.get());
        context->SetConstantBuffer(0, renderer.m_skyboxConstantBuffer.get(), 0);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Sky);
        context->DrawInstanced(3, 1, 0, 0);
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Sky);
    }
}
}