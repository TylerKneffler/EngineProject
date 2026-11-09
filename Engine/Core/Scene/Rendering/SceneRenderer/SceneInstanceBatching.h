#pragma once

#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Rendering/InstanceTilePreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

namespace Engine::Scene::Renderer
{
class SceneInstanceBatching
{
public:
    struct Result
    {
        std::vector<Engine::Rendering::DrawBatchPreparation::Group> automaticGroups;
        std::vector<Engine::Rendering::InstanceTilePreparation::RecordsOwner> instanceTiles;
        Engine::Graphics::IGraphicsBuffer* residentPlacementPool = nullptr;
    };

    static glm::vec2 ScaleShadowLodDistances(glm::vec2 distances, bool shadowOnly);
    static Result Prepare(Scene& scene, std::vector<PreparedDraw>& preparedDraws,
        bool shadowOnly, bool automaticInstancing, bool hasPortalViews,
        bool includeEditorVisuals);
};
}
