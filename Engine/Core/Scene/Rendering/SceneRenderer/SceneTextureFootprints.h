#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Rendering/TextureFootprintCache.h"

namespace Engine::Scene::Renderer
{
class SceneTextureFootprints
{
public:
    // `view` identifies the camera/viewport so cached instance results never
    // mix between views. A null cache re-projects every instance each call.
    static void Request(const std::vector<PreparedDraw>& draws,
        const glm::mat4& viewProjection, uint32_t viewportWidth, uint32_t viewportHeight,
        const std::array<glm::mat4, 4>& shadowViewProjections,
        uint32_t shadowCascadeCount, uint32_t shadowResolution,
        Engine::Rendering::TextureFootprintCache* cache = nullptr, uintptr_t view = 0);
};
}
