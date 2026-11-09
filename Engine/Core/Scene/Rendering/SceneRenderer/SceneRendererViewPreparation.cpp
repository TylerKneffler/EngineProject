#include "Core/Scene/Rendering/SceneRenderer/SceneRendererViewPreparation.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"

#include "Core/Rendering/RenderSpatialIndex.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Engine::Scene::Renderer
{
namespace
{
void HashOcclusionBytes(uint64_t& signature, const void* source, size_t byteCount)
{
    const auto* bytes = static_cast<const uint8_t*>(source);
    for (size_t index = 0; index < byteCount; ++index)
    {
        signature ^= bytes[index];
        signature *= 1099511628211ull;
    }
}
}

float SceneRendererViewPreparation::ComputeNearestLightingDistanceSquared(
    const Scene::FrameRenderItem& item,
    const glm::mat4& itemWorld,
    const glm::vec3& cameraPosition)
{
    const glm::vec3 centerDelta = glm::vec3(itemWorld[3]) - cameraPosition;
    if (!item.mesh || item.sprite || !item.hasBounds)
        return glm::dot(centerDelta, centerDelta);

    const glm::vec3 minimum = item.boundsMinimum;
    const glm::vec3 maximum = item.boundsMaximum;
    const auto bounds = Engine::Rendering::TransformBounds(minimum, maximum, itemWorld);
    const glm::vec3 nearest = glm::clamp(cameraPosition, bounds.minimum, bounds.maximum);
    const glm::vec3 delta = nearest - cameraPosition;
    return glm::dot(delta, delta);
}

bool SceneRendererViewPreparation::IsOutsideViewFrustum(
    const Scene::FrameRenderItem& item,
    const glm::mat4& itemWorld,
    const glm::mat4& viewProjection)
{
    if (!item.mesh || item.sprite || item.warpedVertexBuffer || !item.hasBounds || item.traversalChartPortal)
        return false;

    const glm::vec3 minimum = item.boundsMinimum;
    const glm::vec3 maximum = item.boundsMaximum;
    const glm::mat4 localToClip = viewProjection * itemWorld;
    std::array<glm::vec4, 8> corners{};
    size_t cornerIndex = 0;
    for (int z = 0; z < 2; ++z)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                corners[cornerIndex++] = localToClip * glm::vec4(
                    x ? maximum.x : minimum.x,
                    y ? maximum.y : minimum.y,
                    z ? maximum.z : minimum.z, 1.f);
            }
        }
    }
    const auto allOutside = [&](const auto& predicate)
    {
        return std::all_of(corners.begin(), corners.end(), predicate);
    };
    return allOutside([](const glm::vec4& point)
        { return point.x < -point.w; }) ||
        allOutside([](const glm::vec4& point)
        { return point.x > point.w; }) ||
        allOutside([](const glm::vec4& point)
        { return point.y < -point.w; }) ||
        allOutside([](const glm::vec4& point)
        { return point.y > point.w; }) ||
        allOutside([](const glm::vec4& point)
        { return point.z < 0.f; }) ||
        allOutside([](const glm::vec4& point)
        { return point.z > point.w; });
}

bool SceneRendererViewPreparation::ResolveCameraUsesSourceWarpChart(
    const Scene& scene,
    const Engine::Components::Camera* camera,
    const glm::mat4& authoredCameraWorld,
    bool cameraInsideInteriorOnlyWarp,
    bool cameraOverride,
    bool editorMode2D,
    const glm::mat4& proj,
    const glm::mat4& view,
    const std::vector<Engine::Components::SpatialManipulator*>& spatialManipulators,
    bool sceneCameraWarpLookThrough)
{
    if (!camera || !camera->Owner)
        return false;

    const bool sourceChartAllowed = cameraOverride || sceneCameraWarpLookThrough;
    bool usesSourceWarpChart = !cameraInsideInteriorOnlyWarp &&
        sourceChartAllowed &&
        scene.SampleSpatialPoint(glm::vec3(authoredCameraWorld[3]),
            { Scene::SpatialQueryDomain::Camera, camera->Owner }).affectedByWarpVolume;

    if (!usesSourceWarpChart && !cameraInsideInteriorOnlyWarp && !editorMode2D)
    {
        const auto volumeIntersectsView = [&](const Engine::Components::SpatialManipulator& volume)
        {
            if (!volume.Owner || !volume.definesWarpVolume || !volume.enabled)
                return false;
            const auto shape = static_cast<Engine::Components::SpatialManipulator::WarpVolumeShape>(volume.warpVolumeShape);
            if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Infinite)
                return true;
            const float radius = std::max(0.001f, std::abs(volume.warpVolumeRadius));
            const glm::vec3 halfSize = shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Sphere
                ? glm::vec3(radius)
                : glm::max(glm::abs(volume.warpVolumeSize) * 0.5f, glm::vec3(0.0001f));
            const glm::mat4 localToClip = proj * view * volume.Owner->transform.GetWorldMatrix();
            std::array<glm::vec4, 8> corners{};
            size_t cornerIndex = 0;
            for (int z = 0; z < 2; ++z)
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x)
                        corners[cornerIndex++] = localToClip * glm::vec4(
                            x ? halfSize.x : -halfSize.x,
                            y ? halfSize.y : -halfSize.y,
                            z ? halfSize.z : -halfSize.z, 1.f);
            const auto allOutside = [&](const auto& predicate)
            {
                return std::all_of(corners.begin(), corners.end(), predicate);
            };
            return !(allOutside([](const glm::vec4& point) { return point.x < -point.w; }) ||
                allOutside([](const glm::vec4& point) { return point.x > point.w; }) ||
                allOutside([](const glm::vec4& point) { return point.y < -point.w; }) ||
                allOutside([](const glm::vec4& point) { return point.y > point.w; }) ||
                allOutside([](const glm::vec4& point) { return point.z < 0.f; }) ||
                allOutside([](const glm::vec4& point) { return point.z > point.w; }));
        };
        for (const auto* volume : spatialManipulators)
            if (volume && volume->Owner && volume->Owner->IsEnabledInHierarchy() &&
                (sourceChartAllowed || volume->renderWarpInteriorOnly) && volumeIntersectsView(*volume))
            {
                usesSourceWarpChart = true;
                break;
            }
    }
    return usesSourceWarpChart;
}

void SceneRendererViewPreparation::PrepareVisibleItems(
    const Scene& scene,
    const std::vector<Scene::FrameRenderItem>& frameRenderItems,
    const std::vector<uint8_t>& spatialVisible,
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
    std::vector<uint8_t>* accepted,
    std::vector<uint8_t>* terrainRejected,
    std::vector<uint8_t>* solidRejected)
{
    renderObjects.clear();
    renderObjects.reserve(frameRenderItems.size());

    const size_t itemCount = frameRenderItems.size();
    std::vector<Scene::ViewRenderItem> classified(itemCount);
    std::vector<uint8_t> localAccepted(itemCount, 0),
        localTerrainRejected(itemCount, 0),
        localSolidRejected(itemCount, 0);

    Engine::Rendering::RenderJobs::Get().For(itemCount,
        cameraUsesSourceWarpChart ? SIZE_MAX : 128,
        [&](size_t index)
        {
            const auto& item = frameRenderItems[index];
            if (shadowOnly && (!item.mesh || !item.castsShadows)) return;
            if (!spatialVisible.empty() && !spatialVisible[item.objectSlot]) return;
            if (item.belongsToPreview && !includeEditorVisuals) return;
            const auto world = useSourceChartMesh(item) ? sourceChartWorld(item) : item.world;
            if (!shadowOnly && !hasPortalViews && spatialVisible.empty() && outsideViewFrustum(item, world)) return;
            if (hiddenByTerrain(item)) { localTerrainRejected[index] = 1; return; }
            if (hiddenBySolid(item)) { localSolidRejected[index] = 1; return; }
            const auto delta = glm::vec3(world[3]) - cameraPosition;
            classified[index] = { &item, glm::dot(delta, delta),
                shadowOnly ? 0.f : nearestLightingDistanceSquared(item, world),
                world[3].z, item.sortingLayer,
                item.blended && !(includeEditorVisuals && item.mesh == scene.m_editorWeightPaintMesh) };
            localAccepted[index] = 1;
        });

    if (accepted) accepted->assign(itemCount, 0);
    if (terrainRejected) terrainRejected->assign(itemCount, 0);
    if (solidRejected) solidRejected->assign(itemCount, 0);

    for (size_t index = 0; index < classified.size(); ++index)
    {
        if (localAccepted[index])
            renderObjects.push_back(classified[index]);
        if (accepted) (*accepted)[index] = localAccepted[index];
        if (terrainRejected) (*terrainRejected)[index] = localTerrainRejected[index];
        if (solidRejected) (*solidRejected)[index] = localSolidRejected[index];
    }
}

SceneRendererViewPreparation::PreparedVisibility SceneRendererViewPreparation::PrepareVisibleSet(
    const Scene& scene,
    const std::vector<Scene::FrameRenderItem>& frameRenderItems,
    const std::vector<uint8_t>& spatialVisible,
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
    const std::function<glm::mat4(const Scene::FrameRenderItem&)>& sourceChartWorld)
{
    PreparedVisibility result{};
    result.accepted.assign(frameRenderItems.size(), 0u);
    result.terrainRejected.assign(frameRenderItems.size(), 0u);
    result.solidRejected.assign(frameRenderItems.size(), 0u);

    PrepareVisibleItems(
        scene,
        frameRenderItems,
        spatialVisible,
        cameraPosition,
        shadowOnly,
        includeEditorVisuals,
        hasPortalViews,
        editorMode2D,
        cameraUsesSourceWarpChart,
        hiddenByTerrain,
        hiddenBySolid,
        outsideViewFrustum,
        nearestLightingDistanceSquared,
        useSourceChartMesh,
        sourceChartWorld,
        result.renderObjects,
        &result.accepted,
        &result.terrainRejected,
        &result.solidRejected);

    for (size_t index = 0; index < result.terrainRejected.size(); ++index)
    {
        result.terrainOcclusionCulled += result.terrainRejected[index];
        result.solidOcclusionCulled += result.solidRejected[index];
    }

    SortViewItems(result.renderObjects, editorMode2D);
    return result;
}

void SceneRendererViewPreparation::SortViewItems(
    std::vector<Scene::ViewRenderItem>& renderObjects,
    bool editorMode2D)
{
    std::stable_sort(renderObjects.begin(), renderObjects.end(),
        [editorMode2D](const Scene::ViewRenderItem& first,
            const Scene::ViewRenderItem& second)
        {
            if (editorMode2D)
            {
                if (first.sortingLayer != second.sortingLayer)
                    return first.sortingLayer < second.sortingLayer;
                return first.worldDepth < second.worldDepth;
            }
            if (first.blended != second.blended)
                return !first.blended;
            return first.blended
                ? first.cameraDistanceSquared > second.cameraDistanceSquared
                : first.cameraDistanceSquared < second.cameraDistanceSquared;
        });
}

uint64_t SceneRendererViewPreparation::BuildOcclusionSignature(
    const std::vector<Scene::ViewRenderItem>& renderObjects,
    const glm::mat4& viewProjection,
    const std::function<bool(const Scene::FrameRenderItem&)>& useSourceChartMesh,
    const std::function<glm::mat4(const Scene::FrameRenderItem&)>& sourceChartWorld)
{
    uint64_t occlusionSignature = 1469598103934665603ull;
    HashOcclusionBytes(occlusionSignature, &viewProjection, sizeof(viewProjection));
    for (const Scene::ViewRenderItem& visible : renderObjects)
    {
        HashOcclusionBytes(occlusionSignature, &visible.source->object,
            sizeof(visible.source->object));
        HashOcclusionBytes(occlusionSignature, &visible.blended,
            sizeof(visible.blended));
        const glm::mat4 objectWorld = useSourceChartMesh(*visible.source)
            ? sourceChartWorld(*visible.source)
            : visible.source->world;
        HashOcclusionBytes(occlusionSignature, &objectWorld, sizeof(objectWorld));
        if (visible.source->mesh)
        {
            const uint64_t revision = visible.source->mesh->GetConfigurationRevision();
            HashOcclusionBytes(occlusionSignature, &revision, sizeof(revision));
            const auto fadeRevision = visible.source->mesh->GetRuntimeFadeRevision();
            HashOcclusionBytes(occlusionSignature, &fadeRevision, sizeof(fadeRevision));
        }
    }
    return occlusionSignature;
}
}
