#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererGeometrySubmission.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLodSelection.h"
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalPassPreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneSpatialDebugGeometry.h"

#include <functional>
#include <vector>

namespace Engine::Scene::Renderer
{
struct ScenePortalRenderingContext
{
    using AutomaticGroup = Engine::Rendering::DrawBatchPreparation::Group;
    using SubmitAutomaticGroup = std::function<SubmittedGeometry(
        const AutomaticGroup&, const glm::mat4&, uint32_t,
        Engine::Graphics::IPipelineState*, const std::vector<uint8_t>*)>;

    Scene& scene;
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
    ScenePortalPassPreparation::TargetResolver resolvePortalTarget;
};

class ScenePortalRendering
{
public:
    static void Render(ScenePortalRenderingContext& input);
};
}
