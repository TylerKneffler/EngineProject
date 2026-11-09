#pragma once

#include "Core/Model/GraphicsQualitySettings.h"
#include "Core/Model/LightingData.h"
#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Scene/Scene.h"

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace Engine::Scene::Renderer
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
    };

    struct PrepareResult
    {
        ShadowCascadeSet cascades{};
        std::array<uint8_t, 4> dirtyMask{};
        bool renderMainDirectionalShadow = false;
        bool preserveDirectionalShadowAtlas = false;
    };

    static ShadowCascadeSet BuildCascades(Scene& scene,
        const Engine::Model::RealtimeShadowSelection& shadowSelection,
        const Engine::Model::GraphicsQualityValues& quality,
        const Engine::Components::Camera* cam, const glm::mat4& view,
        uint32_t atlasResolution, uint32_t requestedCascadeCount);

    static PrepareResult PrepareDirectionalShadowCascades(
        Scene& scene,
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
