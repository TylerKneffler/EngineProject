#include "Core/Scene/Rendering/SceneRenderer/SceneRendererVisibility.h"

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace Engine::Scene::Renderer
{
namespace
{
constexpr float kTerrainHorizonCircle = 6.28318530718f;

std::pair<float, float> RayDistances(
    const TerrainFootprint& footprint,
    float angle,
    const glm::vec3& cameraPosition)
{
    const float dx = std::cos(angle);
    const float dz = std::sin(angle);
    const auto slab = [](float direction, float low, float high)
    {
        if (direction == 0.f)
            return std::pair<float, float>{
                -std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity() };
        const float a = low / direction;
        const float b = high / direction;
        return std::pair<float, float>{ std::min(a, b), std::max(a, b) };
    };
    const auto x = slab(dx, footprint.minX - cameraPosition.x,
        footprint.maxX - cameraPosition.x);
    const auto z = slab(dz, footprint.minZ - cameraPosition.z,
        footprint.maxZ - cameraPosition.z);
    return { std::max(x.first, z.first), std::min(x.second, z.second) };
}

template<typename Item>
std::optional<TerrainFootprint> TerrainFootprintForItem(
    const Item& item,
    const glm::vec3& cameraPosition)
{
    if (!item.mesh || !item.hasBounds || item.sprite ||
        item.warpedVertexBuffer || item.traversalChartPortal ||
        item.blended)
        return std::nullopt;
    const glm::mat4& world = item.world;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            if (std::abs(world[column][row] - (column == row ? 1.f : 0.f)) > 0.0001f)
                return std::nullopt;
    const glm::vec3 minimum = item.boundsMinimum + glm::vec3(world[3]);
    const glm::vec3 maximum = item.boundsMaximum + glm::vec3(world[3]);
    if (minimum.x >= maximum.x || minimum.z >= maximum.z ||
        (cameraPosition.x >= minimum.x && cameraPosition.x <= maximum.x &&
            cameraPosition.z >= minimum.z && cameraPosition.z <= maximum.z))
        return std::nullopt;
    const float nearX = std::clamp(cameraPosition.x, minimum.x, maximum.x) - cameraPosition.x;
    const float nearZ = std::clamp(cameraPosition.z, minimum.z, maximum.z) - cameraPosition.z;
    const float nearDistance = std::hypot(nearX, nearZ);
    std::array<float, 4> angles{};
    float farDistance = 0.f;
    int corner = 0;
    for (float z : { minimum.z, maximum.z })
        for (float x : { minimum.x, maximum.x })
        {
            const float dx = x - cameraPosition.x;
            const float dz = z - cameraPosition.z;
            float angle = std::atan2(dz, dx);
            if (angle < 0.f)
                angle += kTerrainHorizonCircle;
            angles[corner++] = angle;
            farDistance = std::max(farDistance, std::hypot(dx, dz));
        }
    std::sort(angles.begin(), angles.end());
    int gapAfter = 3;
    float largestGap = angles[0] + kTerrainHorizonCircle - angles[3];
    for (int i = 0; i < 3; ++i)
    {
        const float gap = angles[i + 1] - angles[i];
        if (gap > largestGap)
        {
            largestGap = gap;
            gapAfter = i;
        }
    }
    const float start = angles[(gapAfter + 1) % 4];
    return TerrainFootprint{
        minimum.x, maximum.x, minimum.z, maximum.z,
        minimum.y, maximum.y,
        nearDistance, farDistance,
        start, start + kTerrainHorizonCircle - largestGap
    };
}
}

std::optional<TerrainFootprint> SceneRendererVisibility::BuildTerrainFootprint(
    const Scene::FrameRenderItem& item,
    const glm::vec3& cameraPosition)
{
    return TerrainFootprintForItem(item, cameraPosition);
}

bool SceneRendererVisibility::HiddenByTerrain(
    const Scene::FrameRenderItem& item,
    const glm::vec3& cameraPosition,
    const std::vector<TerrainFootprint>& terrainBlockers)
{
    if (terrainBlockers.empty())
        return false;

    const auto footprint = BuildTerrainFootprint(item, cameraPosition);
    if (!footprint)
        return false;

    const float height = footprint->maxY - cameraPosition.y;
    const float targetSlope = height / (height >= 0.f ? footprint->nearDistance : footprint->farDistance);
    const float lowHeight = footprint->minY - cameraPosition.y;
    const float targetLowSlope = lowHeight / (lowHeight >= 0.f ? footprint->farDistance : footprint->nearDistance);

    for (const TerrainFootprint& blocker : terrainBlockers)
    {
        if (blocker.farDistance + 0.01f >= footprint->nearDistance)
            continue;
        float start = footprint->angleStart;
        float end = footprint->angleEnd;
        if (start < blocker.angleStart)
        {
            start += kTerrainHorizonCircle;
            end += kTerrainHorizonCircle;
        }
        if (start < blocker.angleStart + 0.00001f ||
            end > blocker.angleEnd - 0.00001f)
            continue;
        std::array<float, 14> angles{};
        size_t angleCount = 0;
        angles[angleCount++] = start;
        angles[angleCount++] = end;
        for (float z : { blocker.minZ, blocker.maxZ })
            for (float x : { blocker.minX, blocker.maxX })
            {
                float angle = std::atan2(z - cameraPosition.z, x - cameraPosition.x);
                if (angle < 0.f)
                    angle += kTerrainHorizonCircle;
                if (angle < start)
                    angle += kTerrainHorizonCircle;
                if (angle > start && angle < end)
                    angles[angleCount++] = angle;
            }
        for (int quadrant = 0; quadrant <= 8; ++quadrant)
        {
            const float angle = quadrant * kTerrainHorizonCircle * 0.25f;
            if (angle > start && angle < end)
                angles[angleCount++] = angle;
        }
        float entryMin = std::numeric_limits<float>::max();
        float entryMax = 0.f;
        float exitMin = std::numeric_limits<float>::max();
        float exitMax = 0.f;
        for (size_t i = 0; i < angleCount; ++i)
        {
            const auto [entry, leave] = RayDistances(blocker, angles[i], cameraPosition);
            entryMin = std::min(entryMin, entry);
            entryMax = std::max(entryMax, entry);
            exitMin = std::min(exitMin, leave);
            exitMax = std::max(exitMax, leave);
        }
        const float highHeight = blocker.maxY - cameraPosition.y;
        const float lowBlockerHeight = blocker.minY - cameraPosition.y;
        const float entrySlope = highHeight / (highHeight >= 0.f ? entryMin : entryMax);
        const float exitSlope = lowBlockerHeight / (lowBlockerHeight >= 0.f ? exitMax : exitMin);
        if (targetLowSlope > entrySlope + 0.001f &&
            targetSlope + 0.001f < exitSlope)
            return true;
    }
    return false;
}

bool SceneRendererVisibility::HiddenBySolid(
    const Scene::FrameRenderItem& item,
    const glm::vec3& cameraPosition,
    const std::vector<TerrainFootprint>& solidBlockers)
{
    if (solidBlockers.empty())
        return false;

    const auto footprint = BuildTerrainFootprint(item, cameraPosition);
    if (!footprint)
        return false;

    const float height = footprint->maxY - cameraPosition.y;
    const float targetSlope = height / (height >= 0.f ? footprint->nearDistance : footprint->farDistance);
    const float lowHeight = footprint->minY - cameraPosition.y;
    const float targetLowSlope = lowHeight / (lowHeight >= 0.f ? footprint->farDistance : footprint->nearDistance);

    for (const TerrainFootprint& blocker : solidBlockers)
    {
        if (blocker.farDistance + 0.01f >= footprint->nearDistance)
            continue;
        float start = footprint->angleStart;
        float end = footprint->angleEnd;
        if (start < blocker.angleStart)
        {
            start += kTerrainHorizonCircle;
            end += kTerrainHorizonCircle;
        }
        if (start < blocker.angleStart + 0.00001f ||
            end > blocker.angleEnd - 0.00001f)
            continue;
        std::array<float, 14> angles{};
        size_t angleCount = 0;
        angles[angleCount++] = start;
        angles[angleCount++] = end;
        for (float z : { blocker.minZ, blocker.maxZ })
            for (float x : { blocker.minX, blocker.maxX })
            {
                float angle = std::atan2(z - cameraPosition.z, x - cameraPosition.x);
                if (angle < 0.f)
                    angle += kTerrainHorizonCircle;
                if (angle < start)
                    angle += kTerrainHorizonCircle;
                if (angle > start && angle < end)
                    angles[angleCount++] = angle;
            }
        for (int quadrant = 0; quadrant <= 8; ++quadrant)
        {
            const float angle = quadrant * kTerrainHorizonCircle * 0.25f;
            if (angle > start && angle < end)
                angles[angleCount++] = angle;
        }
        float entryMin = std::numeric_limits<float>::max();
        float entryMax = 0.f;
        float exitMin = std::numeric_limits<float>::max();
        float exitMax = 0.f;
        for (size_t i = 0; i < angleCount; ++i)
        {
            const auto [entry, leave] = RayDistances(blocker, angles[i], cameraPosition);
            entryMin = std::min(entryMin, entry);
            entryMax = std::max(entryMax, entry);
            exitMin = std::min(exitMin, leave);
            exitMax = std::max(exitMax, leave);
        }
        const float highHeight = blocker.maxY - cameraPosition.y;
        const float lowBlockerHeight = blocker.minY - cameraPosition.y;
        const float entrySlope = highHeight / (highHeight >= 0.f ? entryMin : entryMax);
        const float exitSlope = lowBlockerHeight / (lowBlockerHeight >= 0.f ? exitMax : exitMin);
        if (targetLowSlope > entrySlope + 0.001f &&
            targetSlope + 0.001f < exitSlope)
            return true;
    }
    return false;
}

void SceneRendererVisibility::PrepareTerrainOcclusion(
    Scene& scene,
    const std::vector<Scene::FrameRenderItem>& frameRenderItems,
    const glm::vec3& cameraPosition,
    bool shadowOnly,
    bool includeEditorVisuals,
    bool hasPortalViews,
    bool editorMode2D,
    bool terrainWarpActive,
    std::vector<TerrainFootprint>& terrainBlockers,
    std::vector<TerrainFootprint>& solidBlockers)
{
    const bool useTerrainOcclusion = !shadowOnly && !includeEditorVisuals &&
        !hasPortalViews && !editorMode2D && !terrainWarpActive;
    const bool terrainOcclusionEnabled = useTerrainOcclusion &&
        ReadEnvFlag("ENGINE_VISIBILITY_TERRAIN_OCCLUSION", true);
    const bool solidOcclusionEnabled = useTerrainOcclusion &&
        ReadEnvFlag("ENGINE_VISIBILITY_SOLID_OCCLUSION", false);
    if (!terrainOcclusionEnabled && !solidOcclusionEnabled)
    {
        terrainBlockers.clear();
        solidBlockers.clear();
        return;
    }

    terrainBlockers.clear();
    solidBlockers.clear();
    terrainBlockers.reserve(frameRenderItems.size());
    solidBlockers.reserve(frameRenderItems.size());

    for (const auto& item : frameRenderItems)
    {
        if (!item.object || item.belongsToPreview ||
            (scene.m_previewObject && IsObjectOrDescendant(item.object, scene.m_previewObject)))
            continue;
        const auto footprint = TerrainFootprintForItem(item, cameraPosition);
        if (!footprint)
            continue;
        if (terrainOcclusionEnabled && item.terrainSurface &&
            (item.completeTerrainFootprint || item.terrainCell))
            terrainBlockers.push_back(*footprint);
        if (!solidOcclusionEnabled || !item.mesh || item.terrainSurface ||
            item.blended || !item.material ||
            item.material->doubleSided ||
            item.material->GetAlphaMode() !=
                Engine::Components::MaterialAlphaMode::Opaque ||
            item.mesh->grassShadowCaster ||
            item.mesh->foliageWindStrength > 0.f)
            continue;
        solidBlockers.push_back(*footprint);
    }
}
}
