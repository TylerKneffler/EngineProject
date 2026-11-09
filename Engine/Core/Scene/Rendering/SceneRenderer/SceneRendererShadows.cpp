#include "Core/Scene/Rendering/SceneRenderer/SceneRendererShadows.h"

#include "Core/Rendering/StableShadowCascade.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Engine::Scene::Renderer
{
SceneRendererShadows::ShadowCascadeSet SceneRendererShadows::BuildCascades(
    Scene& scene, const Engine::Model::RealtimeShadowSelection& shadowSelection,
    const Engine::Model::GraphicsQualityValues& quality,
    const Engine::Components::Camera* cam, const glm::mat4& cascadeView,
    uint32_t atlasResolution, uint32_t requestedCascadeCount)
{
        ShadowCascadeSet out{};
        const float shadowDistance = std::clamp(
            scene.m_realtimeShadowSettings.directionalDistance * quality.shadows,
            1.f, 100000.f);
        out.count = std::clamp(requestedCascadeCount, 1u, 4u);
        const float cameraNear = std::max(0.001f, cam->nearPlane);
        const float cameraFar = std::max(cameraNear + 0.001f, cam->farPlane);
        const float cascadeFar = std::min(cameraFar,
            std::max(cameraNear + 0.001f, shadowDistance));
        const float splitLambda = std::clamp(
            scene.m_realtimeShadowSettings.cascadeSplitLambda, 0.f, 1.f);
        for (uint32_t cascade = 0; cascade < out.count; ++cascade)
        {
            const float fraction = static_cast<float>(cascade + 1u) /
                static_cast<float>(out.count);
            const float logarithmic = cameraNear * std::pow(
                cascadeFar / cameraNear, fraction);
            const float uniform = cameraNear +
                (cascadeFar - cameraNear) * fraction;
            out.splits[cascade] = glm::mix(uniform, logarithmic, splitLambda);
        }
        for (uint32_t cascade = out.count; cascade < 4u; ++cascade)
            out.splits[cascade] = cascadeFar;

        const auto cascadePosition = glm::vec3(glm::inverse(cascadeView)[3]);
        const uint32_t tileResolution = out.count > 1u
            ? std::max(1u, atlasResolution / 2u)
            : atlasResolution;
        for (uint32_t cascade = 0; cascade < out.count; ++cascade)
        {
            const auto stable = Engine::Rendering::BuildStableShadowCascade(
                cascadePosition,
                shadowSelection.directionToLight,
                out.splits[cascade],
                shadowDistance,
                tileResolution);
            out.centers[cascade] = stable.center;
            out.radii[cascade] = stable.radius;
            out.viewProjections[cascade] = stable.viewProjection;
        }
        out.data = {
            static_cast<float>(out.count),
            out.count > 1u ? 0.5f : 1.f,
            std::clamp(scene.m_realtimeShadowSettings.cascadeTransitionFraction,
                0.f, 0.3f),
            out.splits[out.count - 1u] };
        out.cameraData = glm::vec4(glm::normalize(glm::vec3(
            glm::inverse(cascadeView)[2])), cameraNear);
        return out;
}

SceneRendererShadows::PrepareResult SceneRendererShadows::PrepareDirectionalShadowCascades(
    Scene& scene,
    Engine::Graphics::IGraphicsContext* context,
    const Engine::Model::RealtimeShadowSelection& shadowSelection,
    const glm::mat4& view,
    const glm::vec3& cameraPosition,
    const Engine::Model::GraphicsQualityValues& quality,
    const Engine::Components::Camera* cam,
    bool shadowOnly)
{
    ShadowCascadeSet mainShadowCascades{};
    bool renderMainDirectionalShadow = shadowSelection.valid;
    std::array<uint8_t, 4> directionalShadowDirtyMask{};
    bool preserveDirectionalShadowAtlas = false;
    if (shadowSelection.valid)
    {
        ++scene.m_directionalShadowFrameCounter;
        const ShadowCascadeSet requestedShadowCascades = BuildCascades(
            scene, shadowSelection, quality, cam, view, scene.m_directionalShadowResolution,
            scene.m_realtimeShadowSettings.directionalCascadeCount);
        mainShadowCascades = requestedShadowCascades;
        const bool cacheReusable = scene.m_directionalShadowCacheValid &&
            scene.m_directionalShadowCachedQuality.geometry == quality.geometry &&
            scene.m_directionalShadowCachedQuality.shadows == quality.shadows &&
            scene.m_directionalShadowCachedQuality.texture == quality.texture &&
            scene.m_directionalShadowCachedQuality.material == quality.material &&
            scene.m_directionalShadowCachedResolution ==
                scene.m_directionalShadowResolution &&
            scene.m_directionalShadowCachedLightKey ==
                shadowSelection.stableLightKey &&
            scene.m_directionalShadowCachedCascadeCount ==
                requestedShadowCascades.count &&
            // A shared light direction cannot describe tiles rendered with
            // different orientations. Only tolerate normalization roundoff;
            // sun motion must invalidate every cascade together.
            glm::length(glm::normalize(shadowSelection.directionToLight) -
                scene.m_directionalShadowCachedDirection) <= 1.e-6f;
        if (shadowOnly)
        {
            bool staggerMovingCascades = false;
            auto copyCachedCascades = [&]()
            {
                mainShadowCascades.viewProjections =
                    scene.m_directionalShadowCachedViewProjections;
                mainShadowCascades.splits =
                    scene.m_directionalShadowCachedCascadeSplits;
                mainShadowCascades.data =
                    scene.m_directionalShadowCachedCascadeData;
                mainShadowCascades.cameraData =
                    scene.m_directionalShadowCachedCameraData;
                mainShadowCascades.centers =
                    scene.m_directionalShadowCachedCascadeCenters;
                mainShadowCascades.radii =
                    scene.m_directionalShadowCachedCascadeRadii;
                mainShadowCascades.count =
                    scene.m_directionalShadowCachedCascadeCount;
            };
            if (!cacheReusable)
            {
                for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    directionalShadowDirtyMask[cascade] = 1u;
            }
            else
            {
                const bool globalCascadeDataChanged =
                    glm::length(requestedShadowCascades.splits -
                        scene.m_directionalShadowCachedCascadeSplits) > 1e-4f ||
                    glm::length(requestedShadowCascades.data -
                        scene.m_directionalShadowCachedCascadeData) > 1e-4f ||
                    std::abs(requestedShadowCascades.cameraData.w -
                        scene.m_directionalShadowCachedCameraData.w) > 1e-4f;
                // Rotation and lens changes do not alter radial coverage.
                // Stagger walking updates only while every retained tile can
                // still cover its entire current receiver sphere.
                staggerMovingCascades = !globalCascadeDataChanged &&
                    glm::length(cameraPosition-scene.m_directionalShadowCachedCameraPosition)<=1.f;
                for(uint32_t cascade=0;cascade<requestedShadowCascades.count;++cascade)
                    if(glm::length(cameraPosition-scene.m_directionalShadowCachedCascadeCenters[cascade])+
                        requestedShadowCascades.splits[cascade]>
                        scene.m_directionalShadowCachedCascadeRadii[cascade]-.01f)
                        staggerMovingCascades=false;
                if (globalCascadeDataChanged)
                {
                    for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                        directionalShadowDirtyMask[cascade] = 1u;
                }
                else
                {
                    const auto matrixDelta = [](const glm::mat4& first,
                        const glm::mat4& second)
                    {
                        float maxDelta = 0.f;
                        for (uint32_t row = 0; row < 4u; ++row)
                            for (uint32_t col = 0; col < 4u; ++col)
                                maxDelta = std::max(maxDelta,
                                    std::abs(first[row][col] - second[row][col]));
                        return maxDelta;
                    };
                    for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    {
                        const float centerDelta = glm::length(
                            requestedShadowCascades.centers[cascade] -
                            scene.m_directionalShadowCachedCascadeCenters[cascade]);
                        const float radiusDelta = std::abs(
                            requestedShadowCascades.radii[cascade] -
                            scene.m_directionalShadowCachedCascadeRadii[cascade]);
                        // Range/split changes alter radial coverage; refresh
                        // the layout together rather than staggering it.
                        if(radiusDelta>0.05f) staggerMovingCascades=false;
                        const float projectionDelta = matrixDelta(
                            requestedShadowCascades.viewProjections[cascade],
                            scene.m_directionalShadowCachedViewProjections[cascade]);
                        directionalShadowDirtyMask[cascade] =
                            (centerDelta > 0.05f || radiusDelta > 0.05f ||
                                projectionDelta > 1e-4f) ? 1u : 0u;
                    }
                    const uint32_t refreshCascade =
                        scene.m_directionalShadowRefreshCursor %
                        requestedShadowCascades.count;
                    const bool temporalRefreshDue =
                        (scene.m_directionalShadowFrameCounter -
                            scene.m_directionalShadowCascadeLastRenderedFrame[refreshCascade]) >= 2u;
                    if (temporalRefreshDue)
                    {
                        directionalShadowDirtyMask[refreshCascade] = 1u;
                        scene.m_directionalShadowRefreshCursor =
                            (refreshCascade + 1u) %
                            requestedShadowCascades.count;
                    }
                }
            }
            if (staggerMovingCascades && requestedShadowCascades.count>2u)
            {
                // At most two tiles per frame: prioritize a dirty near tile,
                // then visit far tiles in a rotating order without starvation.
                uint32_t remaining=2u-(directionalShadowDirtyMask[0] ? 1u:0u);
                const uint32_t farCount=requestedShadowCascades.count-1u;
                const uint32_t first=uint32_t(scene.m_directionalShadowFrameCounter%farCount);
                for(uint32_t offset=0;offset<farCount;++offset)
                {
                    const uint32_t cascade=1u+(first+offset)%farCount;
                    if(!directionalShadowDirtyMask[cascade]) continue;
                    if(remaining) --remaining;
                    else directionalShadowDirtyMask[cascade]=0u;
                }
            }
            uint32_t dirtyCascadeCount = 0;
            for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                dirtyCascadeCount += directionalShadowDirtyMask[cascade] ? 1u : 0u;
            if(dirtyCascadeCount && !context->SupportsDepthRectangleClear())
            {
                // Older DX11 drivers cannot clear depth rectangles. Preserve
                // correctness by refreshing the whole atlas on those devices.
                directionalShadowDirtyMask.fill(1u);
                dirtyCascadeCount=requestedShadowCascades.count;
            }
            renderMainDirectionalShadow = dirtyCascadeCount > 0u;
            if (!renderMainDirectionalShadow && cacheReusable)
                copyCachedCascades();
            preserveDirectionalShadowAtlas = cacheReusable &&
                renderMainDirectionalShadow &&
                dirtyCascadeCount < requestedShadowCascades.count;
            if (preserveDirectionalShadowAtlas)
                for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    if (!directionalShadowDirtyMask[cascade])
                    {
                        mainShadowCascades.viewProjections[cascade] =
                            scene.m_directionalShadowCachedViewProjections[cascade];
                        mainShadowCascades.centers[cascade] =
                            scene.m_directionalShadowCachedCascadeCenters[cascade];
                        mainShadowCascades.radii[cascade] =
                            scene.m_directionalShadowCachedCascadeRadii[cascade];
                    }
        }
        else if (scene.m_directionalShadowCacheValid)
        {
            mainShadowCascades.viewProjections =
                scene.m_directionalShadowCachedViewProjections;
            mainShadowCascades.splits =
                scene.m_directionalShadowCachedCascadeSplits;
            mainShadowCascades.data =
                scene.m_directionalShadowCachedCascadeData;
            mainShadowCascades.cameraData =
                scene.m_directionalShadowCachedCameraData;
            mainShadowCascades.centers =
                scene.m_directionalShadowCachedCascadeCenters;
            mainShadowCascades.radii =
                scene.m_directionalShadowCachedCascadeRadii;
            mainShadowCascades.count =
                scene.m_directionalShadowCachedCascadeCount;
        }
    }
    PrepareResult result{};
    result.cascades = mainShadowCascades;
    result.dirtyMask = directionalShadowDirtyMask;
    result.renderMainDirectionalShadow = renderMainDirectionalShadow;
    result.preserveDirectionalShadowAtlas = preserveDirectionalShadowAtlas;
    return result;
}

bool SceneRendererShadows::IsOutsideCascade(
    const PreparedDraw& draw,
    const ShadowCascadeSet& cascades,
    uint32_t cascade)
{
    if (!draw.mesh || !draw.mesh->HasBounds() ||
        (!draw.terrainCell && !draw.mesh->useBoundsForShadowCulling))
        return false;
    const glm::vec3 minimum = draw.mesh->GetBoundsMin();
    const glm::vec3 maximum = draw.mesh->GetBoundsMax();
    if (cascades.receivers[cascade].Outside(
        minimum, maximum, draw.objectData.world))
        return true;
    const glm::vec3 center = (minimum + maximum) * 0.5f;
    const glm::vec3 extent = (maximum - minimum) * 0.5f;
    const glm::mat4 localToClip = cascades.viewProjections[cascade] *
        draw.objectData.world;
    const glm::vec4 xRow(localToClip[0][0], localToClip[1][0],
        localToClip[2][0], localToClip[3][0]);
    const glm::vec4 yRow(localToClip[0][1], localToClip[1][1],
        localToClip[2][1], localToClip[3][1]);
    const glm::vec4 zRow(localToClip[0][2], localToClip[1][2],
        localToClip[2][2], localToClip[3][2]);
    const glm::vec4 wRow(localToClip[0][3], localToClip[1][3],
        localToClip[2][3], localToClip[3][3]);
    const glm::vec4 homogeneousCenter(center, 1.f);
    const auto allOutside = [&](const glm::vec4& plane)
    {
        const float centerValue = glm::dot(plane, homogeneousCenter);
        const float radius = glm::dot(glm::abs(glm::vec3(plane)), extent);
        return centerValue + radius < -1e-4f;
    };
    return allOutside(wRow + xRow) || allOutside(wRow - xRow) ||
        allOutside(wRow + yRow) || allOutside(wRow - yRow) ||
        allOutside(zRow) || allOutside(wRow - zRow);
}
}
