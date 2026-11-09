#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

namespace Engine::Scene::Renderer
{
class SceneTextureFootprints
{
public:
    static void Request(const std::vector<PreparedDraw>& draws,
        const glm::mat4& viewProjection, uint32_t viewportWidth, uint32_t viewportHeight,
        const std::array<glm::mat4, 4>& shadowViewProjections,
        uint32_t shadowCascadeCount, uint32_t shadowResolution);
};
}
