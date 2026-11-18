#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Rendering/SceneRenderer/SceneRendererGeometrySubmission.h"
#include "Core/Rendering/SceneRenderer/SceneRendererLodSelection.h"
#include "Core/Rendering/Portal/PortalPassPreparation.h"
#include "Core/Rendering/Portal/SpatialDebugGeometry.h"

#include <functional>
#include <vector>

namespace Engine::Rendering
{
struct ScenePortalRenderingContext
{
    using AutomaticGroup = Engine::Rendering::DrawBatchPreparation::Group;
    using SubmitAutomaticGroup = std::function<SubmittedGeometry(
        const AutomaticGroup&, const glm::mat4&, uint32_t,
        Engine::Graphics::IPipelineState*, const std::vector<uint8_t>*)>;

    SceneRenderer& renderer;
    Engine::Graphics::IGraphicsContext* graphics;
    std::vector<PreparedDraw>& preparedDraws;
    const std::vector<AutomaticGroup>& automaticGroups;
    Engine::Components::Camera* camera;
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewProjection;
    glm::vec3 cameraPosition;
    uint32_t viewportWidth;
    uint32_t viewportHeight;
    uint32_t portalRenderSlotsPerView;
    uint32_t portalObjectSlotCount;
    bool cameraUsesSourceWarpChart;
    bool includeEditorVisuals;
    const SceneRendererShadows::ShadowCascadeSet& mainShadowCascades;
    SceneRendererGeometrySubmission& geometrySubmission;
    SceneRendererLodSelection& lodSelection;
    SubmitAutomaticGroup submitAutomaticGroup;
    std::function<bool(const PreparedDraw&)> isSpatialManipulatorCarrierDraw;
    std::function<void()> ensureRecordsUploaded;
    PortalPassPreparation::TargetResolver resolvePortalTarget;
};

class ScenePortalRendering
{
public:
    static void Render(ScenePortalRenderingContext& input);
};
}
