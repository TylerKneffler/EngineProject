#pragma once

#include "Core/Rendering/SceneRenderer.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace Engine::Rendering
{
struct TerrainFootprint
{
    float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f, minY = 0.f, maxY = 0.f;
    float nearDistance = 0.f, farDistance = 0.f;
    float angleStart = 0.f, angleEnd = 0.f;
};

class SceneRendererVisibility
{
public:
    static std::optional<TerrainFootprint> BuildTerrainFootprint(
        const SceneRenderer::FrameRenderItem& item,
        const glm::vec3& cameraPosition);

    static bool HiddenByTerrain(
        const SceneRenderer::FrameRenderItem& item,
        const glm::vec3& cameraPosition,
        const std::vector<TerrainFootprint>& terrainBlockers);

    static bool HiddenBySolid(
        const SceneRenderer::FrameRenderItem& item,
        const glm::vec3& cameraPosition,
        const std::vector<TerrainFootprint>& solidBlockers);

    static void PrepareTerrainOcclusion(
        SceneRenderer& renderer,
        const std::vector<SceneRenderer::FrameRenderItem>& frameRenderItems,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        bool includeEditorVisuals,
        bool hasPortalViews,
        bool editorMode2D,
        bool terrainWarpActive,
        std::vector<TerrainFootprint>& terrainBlockers,
        std::vector<TerrainFootprint>& solidBlockers);
};
}
