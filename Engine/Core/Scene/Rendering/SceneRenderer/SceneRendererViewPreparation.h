#pragma once

#include "Core/Scene/Scene.h"

#include <cstddef>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Scene::Renderer
{
class SceneRendererViewPreparation
{
public:
    struct PreparedVisibility
    {
        std::vector<Scene::ViewRenderItem> renderObjects;
        std::vector<uint8_t> terrainRejected;
        std::vector<uint8_t> solidRejected;
        uint32_t terrainOcclusionCulled = 0u;
        uint32_t solidOcclusionCulled = 0u;
    };

    static bool ResolveCameraUsesSourceWarpChart(
        const Scene& scene,
        const Engine::Components::Camera* camera,
        const glm::mat4& authoredCameraWorld,
        bool cameraInsideInteriorOnlyWarp,
        bool cameraOverride,
        bool editorMode2D,
        const glm::mat4& proj,
        const glm::mat4& view,
        const std::vector<Engine::Components::SpatialManipulator*>& spatialManipulators,
        bool sceneCameraWarpLookThrough);

    static void PrepareVisibleItems(
        const Scene& scene,
        const std::vector<Scene::FrameRenderItem>& frameRenderItems,
        const std::vector<uint32_t>& candidateFrameItems,
        bool useSpatialCandidates,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        bool includeEditorVisuals,
        bool hasPortalViews,
        bool editorMode2D,
        bool cameraUsesSourceWarpChart,
        const std::function<bool(const Scene::FrameRenderItem&)>& hiddenByTerrain,
        const std::function<bool(const Scene::FrameRenderItem&)>& hiddenBySolid,
        const std::function<bool(const Scene::FrameRenderItem&, const glm::mat4&)>& outsideViewFrustum,
        const std::function<float(const Scene::FrameRenderItem&, const glm::mat4&)>& nearestLightingDistanceSquared,
        const std::function<bool(const Scene::FrameRenderItem&)>& useSourceChartMesh,
        const std::function<glm::mat4(const Scene::FrameRenderItem&)>& sourceChartWorld,
        std::vector<Scene::ViewRenderItem>& renderObjects,
        std::vector<uint8_t>* terrainRejected = nullptr,
        std::vector<uint8_t>* solidRejected = nullptr);

    static PreparedVisibility PrepareVisibleSet(
        const Scene& scene,
        const std::vector<Scene::FrameRenderItem>& frameRenderItems,
        const std::vector<uint32_t>& candidateFrameItems,
        bool useSpatialCandidates,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        bool includeEditorVisuals,
        bool hasPortalViews,
        bool editorMode2D,
        bool cameraUsesSourceWarpChart,
        const std::function<bool(const Scene::FrameRenderItem&)>& hiddenByTerrain,
        const std::function<bool(const Scene::FrameRenderItem&)>& hiddenBySolid,
        const std::function<bool(const Scene::FrameRenderItem&, const glm::mat4&)>& outsideViewFrustum,
        const std::function<float(const Scene::FrameRenderItem&, const glm::mat4&)>& nearestLightingDistanceSquared,
        const std::function<bool(const Scene::FrameRenderItem&)>& useSourceChartMesh,
        const std::function<glm::mat4(const Scene::FrameRenderItem&)>& sourceChartWorld);

    static float ComputeNearestLightingDistanceSquared(
        const Scene::FrameRenderItem& item,
        const glm::mat4& itemWorld,
        const glm::vec3& cameraPosition);

    static bool IsOutsideViewFrustum(
        const Scene::FrameRenderItem& item,
        const glm::mat4& itemWorld,
        const glm::mat4& viewProjection);

    static void SortViewItems(
        std::vector<Scene::ViewRenderItem>& renderObjects,
        bool editorMode2D);

    static uint64_t BuildOcclusionSignature(
        const std::vector<Scene::ViewRenderItem>& renderObjects,
        const glm::mat4& viewProjection,
        const std::function<bool(const Scene::FrameRenderItem&)>& useSourceChartMesh,
        const std::function<glm::mat4(const Scene::FrameRenderItem&)>& sourceChartWorld);
};
}
