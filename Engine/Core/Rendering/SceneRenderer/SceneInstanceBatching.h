#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Rendering/InstanceTilePreparation.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

namespace Engine::Rendering
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
    static Result Prepare(SceneRenderer& renderer, std::vector<PreparedDraw>& preparedDraws,
        bool shadowOnly, bool automaticInstancing, bool hasPortalViews,
        bool includeEditorVisuals);
};
}
