#pragma once

#include "Core/Model/GraphicsQualitySettings.h"
#include "Core/Model/LightingData.h"
#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Rendering/SceneRenderer.h"

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace Engine::Rendering
{
struct PreparedDraw;
class SceneRendererShadows
{
public:
    struct ShadowCascadeSet
    {
        std::array<glm::mat4, 4> viewProjections{
            glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f), glm::mat4(1.f) };
        glm::vec4 splits { 0.f };
        glm::vec4 data { 0.f };
        glm::vec4 cameraData { 0.f };
        std::array<glm::vec3, 4> centers{};
        std::array<float, 4> radii{};
        std::array<Engine::Rendering::ShadowReceiverCulling, 4> receivers{};
        uint32_t count = 1u;
        // The last cascade is the far range (BuildCascades).
        bool hasFarCascade = false;
        // Atlas tiles and refresh blends, as ObjectGPUData::shadowAtlasTiles
        // and shadowRefreshBlend carry them to the shaders.
        glm::vec4 atlasTiles{ 0.f + 8.f * 1.f + 64.f * 2.f + 512.f * 3.f, -1.f, -1.f, 2.f };
        std::array<glm::vec4, 2> refreshBlends{};
    };

    struct PrepareResult
    {
        ShadowCascadeSet cascades{};
        std::array<uint8_t, 4> dirtyMask{};
        bool renderMainDirectionalShadow = false;
        bool preserveDirectionalShadowAtlas = false;
        // The far cascade is dirty for a region refresh: only the texels in
        // farRegionPixels (x0, y0, x1, y1 within its tile) are cleared and
        // redrawn, through farRegionViewProjection (the cached projection
        // cropped onto them); cascades.viewProjections keeps the whole tile.
        bool farRegionRefresh = false;
        glm::uvec4 farRegionPixels{ 0u };
        glm::mat4 farRegionViewProjection{ 1.f };
    };

    static ShadowCascadeSet BuildCascades(SceneRenderer& renderer,
        const Engine::Model::RealtimeShadowSelection& shadowSelection,
        const Engine::Model::GraphicsQualityValues& quality,
        const Engine::Components::Camera* cam, const glm::mat4& view,
        uint32_t atlasResolution, uint32_t requestedCascadeCount);

    static PrepareResult PrepareDirectionalShadowCascades(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context,
        const Engine::Model::RealtimeShadowSelection& shadowSelection,
        const glm::mat4& view,
        const glm::vec3& cameraPosition,
        const Engine::Model::GraphicsQualityValues& quality,
        const Engine::Components::Camera* cam,
        bool shadowOnly);

    static bool IsOutsideCascade(
        const PreparedDraw& draw,
        const ShadowCascadeSet& cascades,
        uint32_t cascade);
};
}
