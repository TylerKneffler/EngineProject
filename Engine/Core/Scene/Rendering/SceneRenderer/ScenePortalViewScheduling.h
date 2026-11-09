#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Engine::Scene::Renderer
{
    struct PortalStencilPass
    {
        Engine::Components::SpatialManipulator* source = nullptr;
        Engine::Components::SpatialManipulator* target = nullptr;
        float viewDepth = 0.f;
        size_t drawOrder = 0;
        DrawCBData apertureDrawData{};
        ObjectGPUData apertureObjectData{};
        uint64_t apertureVertexOffset = 0;
        uint32_t apertureVertexCount = 0;
    };

    struct PortalViewJob
    {
        const PortalStencilPass* portal = nullptr;
        const Engine::Core::Object* contentScopeRoot = nullptr;
        glm::mat4 apertureView { 1.f };
        glm::mat4 mappedView { 1.f };
        glm::vec3 mappedCameraPosition { 0.f };
        uint32_t depth = 0;
        uint32_t rootStencilBase = 1;
        size_t parentJobIndex = std::numeric_limits<size_t>::max();
        // Constant-buffer records retain globally unique frame slots because
        // deferred backends reference them after command recording.
        uint32_t dataSlotBase = 0;
        // Structured portal data lives in its own compact buffer and starts
        // at zero rather than after the ordinary-view capacity.
        uint32_t objectDataSlotBase = 0;
        uint64_t skyboxConstantBufferOffset = 0;
        uint64_t shadowKey = 0;
        float screenArea = 0.f;
        size_t shadowCacheIndex = std::numeric_limits<size_t>::max();
    };

class ScenePortalViewScheduling
{
public:
    static std::vector<PortalViewJob> Build(Scene& scene,
        const std::vector<PortalStencilPass>& portalPasses,
        const glm::mat4& view, const glm::mat4& proj,
        const Engine::Components::Camera* cam, bool cameraUsesSourceWarpChart);

    static std::optional<Engine::Graphics::IGraphicsContext::ScissorRect> ApertureScissor(
        const PortalStencilPass& pass, const glm::mat4& candidateView,
        const glm::mat4& proj, uint32_t viewportWidth, uint32_t viewportHeight,
        bool cameraUsesSourceWarpChart);
};
}
