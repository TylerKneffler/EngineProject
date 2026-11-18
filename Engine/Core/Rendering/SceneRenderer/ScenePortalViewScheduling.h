#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Rendering/Portal/PortalPasses.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Engine::Rendering
{
class ScenePortalViewScheduling
{
public:
    static std::vector<PortalViewJob> Build(SceneRenderer& renderer,
        const std::vector<PortalStencilPass>& portalPasses,
        const glm::mat4& view, const glm::mat4& proj,
        const Engine::Components::Camera* cam, bool cameraUsesSourceWarpChart);

    static std::optional<Engine::Graphics::IGraphicsContext::ScissorRect> ApertureScissor(
        const PortalStencilPass& pass, const glm::mat4& candidateView,
        const glm::mat4& proj, uint32_t viewportWidth, uint32_t viewportHeight,
        bool cameraUsesSourceWarpChart);
};
}
