#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererShadows.h"

#include "Core/Rendering/StableShadowCascade.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace Engine::Rendering
{
namespace
{
uint64_t MixShadowContent(uint64_t value)
{
    value ^= value >> 33; value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33; value *= 0xc4ceb9fe1a85ec53ull;
    return value ^ (value >> 33);
}

// Signature of the shadow casters inside a cascade's light-space square: an
// order-independent sum over casting items (shared prefab parts: over their
// instance blocks, which carry world bounds) whose world box overlaps it, of
// their identity and instance/geometry revision. It changes when casters
// there stream in or out, are swapped (near/far blocks) or change placements.
// Visits every shadow-casting item (instance blocks of shared prefab parts
// separately) with its world box projected into light clip space:
// visit(minX, maxX, minY, maxY, hash). Orthographic: w = 1; a box's projected
// half extent is |M| * extent.
template<class Items, class Visit>
void ForEachShadowCaster(const Items& items, const glm::mat4& m, Visit&& visit)
{
    const auto project = [&](const glm::vec3& minimum, const glm::vec3& maximum, uint64_t hash)
    {
        const glm::vec3 centre = (minimum + maximum) * .5f, extent = (maximum - minimum) * .5f;
        const glm::vec4 p = m * glm::vec4(centre, 1.f);
        const float ex = std::abs(m[0][0]) * extent.x + std::abs(m[1][0]) * extent.y + std::abs(m[2][0]) * extent.z;
        const float ey = std::abs(m[0][1]) * extent.x + std::abs(m[1][1]) * extent.y + std::abs(m[2][1]) * extent.z;
        visit(p.x - ex, p.x + ex, p.y - ey, p.y + ey, hash);
    };
    for (const auto& item : items)
    {
        const auto* mesh = item.mesh;
        if (!item.castsShadows || !mesh) continue;
        if (mesh->UsesInstanceBlocks())
        {
            for (const auto& block : mesh->GetInstanceBlocks())
                if (block.data)
                    project(block.minimum, block.maximum,
                        MixShadowContent(uint64_t(reinterpret_cast<uintptr_t>(block.data.get())) ^
                        (block.data->revision * 0x9e3779b97f4a7c15ull) ^
                        uint64_t(reinterpret_cast<uintptr_t>(mesh)) * 31u));
            continue;
        }
        if (!item.hasBounds) continue;
        const auto bounds = TransformBounds(item.boundsMinimum, item.boundsMaximum, item.world);
        const auto& instances = mesh->GetInstanceData();
        const uint64_t revision = instances ? instances->revision : mesh->GetAuthoredGeometryRevision();
        project(bounds.minimum, bounds.maximum,
            MixShadowContent(uint64_t(reinterpret_cast<uintptr_t>(mesh)) ^
            (revision * 0x9e3779b97f4a7c15ull)));
    }
}

template<class Items>
uint64_t ShadowCascadeContent(const Items& items, const glm::mat4& viewProjection)
{
    uint64_t signature = 0;
    ForEachShadowCaster(items, viewProjection,
        [&](float x0, float x1, float y0, float y1, uint64_t hash)
        {
            if (x1 >= -1.f && x0 <= 1.f && y1 >= -1.f && y0 <= 1.f)
                signature += hash;
        });
    return signature ? signature : 1u;
}

// The same signature per region of a kFarShadowRegionGrid x kFarShadowRegionGrid
// grid over the cascade's tile (row 0 at the top, as the atlas is addressed).
// A caster adds to every region its light-space box touches.
template<class Items>
SceneRenderer::FarShadowRegionContent ShadowCascadeRegionContent(
    const Items& items, const glm::mat4& viewProjection)
{
    constexpr int grid = int(SceneRenderer::kFarShadowRegionGrid);
    SceneRenderer::FarShadowRegionContent regions{};
    ForEachShadowCaster(items, viewProjection,
        [&](float x0, float x1, float y0, float y1, uint64_t hash)
        {
            if (x1 < -1.f || x0 > 1.f || y1 < -1.f || y0 > 1.f) return;
            const auto cell = [&](float uv)
            { return std::clamp(int(std::floor(uv * grid)), 0, grid - 1); };
            const int c0 = cell(x0 * .5f + .5f), c1 = cell(x1 * .5f + .5f);
            const int r0 = cell(.5f - y1 * .5f), r1 = cell(.5f - y0 * .5f);
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c)
                    regions[size_t(r * grid + c)] += hash;
        });
    return regions;
}

// Far cascade regions to redraw, as an inclusive rectangle of grid cells
// {c0, r0, c1, r1}: the changed region nearest the camera, grown over the
// other changed regions while the rectangle stays within maxCells cells. The
// regions left out stay changed and are picked up on a later frame.
bool ChooseFarShadowRegions(const SceneRenderer::FarShadowRegionContent& current,
    const SceneRenderer::FarShadowRegionContent& cached, glm::vec2 cameraCell,
    uint32_t maxCells, glm::ivec4& rectangle)
{
    constexpr int grid = int(SceneRenderer::kFarShadowRegionGrid);
    std::vector<std::pair<float, int>> changed;
    for (int index = 0; index < grid * grid; ++index)
        if (current[size_t(index)] != cached[size_t(index)])
        {
            const glm::vec2 centre(float(index % grid) + .5f, float(index / grid) + .5f);
            changed.emplace_back(glm::length(centre - cameraCell), index);
        }
    if (changed.empty()) return false;
    std::sort(changed.begin(), changed.end());
    const int first = changed.front().second;
    rectangle = { first % grid, first / grid, first % grid, first / grid };
    for (size_t i = 1; i < changed.size(); ++i)
    {
        const int c = changed[i].second % grid, r = changed[i].second / grid;
        const glm::ivec4 grown{ std::min(rectangle.x, c), std::min(rectangle.y, r),
            std::max(rectangle.z, c), std::max(rectangle.w, r) };
        if (uint32_t((grown.z - grown.x + 1) * (grown.w - grown.y + 1)) <= maxCells)
            rectangle = grown;
    }
    return true;
}

// ENGINE_SHADOW_FAR_REGIONS=0 turns region refresh of the far cascade off
// (A/B runs): it then refreshes whole when casters changed, at most every 240
// shadow frames, as before 2026-11-24.
bool FarShadowRegionRefresh()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_SHADOW_FAR_REGIONS");
        return !(value && value[0] == '0');
    }();
    return enabled;
}
}
using Scene = Engine::Scene::Scene;
SceneRendererShadows::ShadowCascadeSet SceneRendererShadows::BuildCascades(
    SceneRenderer& renderer, const Engine::Model::RealtimeShadowSelection& shadowSelection,
    const Engine::Model::GraphicsQualityValues& quality,
    const Engine::Components::Camera* cam, const glm::mat4& cascadeView,
    uint32_t atlasResolution, uint32_t requestedCascadeCount)
{
        ShadowCascadeSet out{};
        const float authoredDistance = std::clamp(
            renderer.m_realtimeShadowSettings.directionalDistance * quality.shadows,
            1.f, 100000.f);
        // ENGINE_SHADOW_NEAR_DISTANCE=<m>: a longer near range for A/B runs,
        // with the split blend chosen so the first cascade keeps its authored
        // extent (and texel density); the later near cascades stretch.
        static const float nearOverride = []
        { const char* v = std::getenv("ENGINE_SHADOW_NEAR_DISTANCE"); return v ? float(std::atof(v)) : 0.f; }();
        const float shadowDistance = nearOverride > 1.f
            ? std::clamp(nearOverride * quality.shadows, 1.f, 100000.f) : authoredDistance;
        out.count = std::clamp(requestedCascadeCount, 1u, 4u);
        const float cameraNear = std::max(0.001f, cam->nearPlane);
        const float cameraFar = std::max(cameraNear + 0.001f, cam->farPlane);
        const float cascadeFar = std::min(cameraFar,
            std::max(cameraNear + 0.001f, shadowDistance));
        // Far range: the last cascade alone covers shadowDistance .. far, so
        // distant hills and forests keep their shadows (see
        // RealtimeShadowSettings::directionalFarDistance).
        const float farDistance = std::min(cameraFar,
            renderer.m_realtimeShadowSettings.directionalFarDistance * quality.shadows);
        out.hasFarCascade = out.count >= 2u && farDistance > cascadeFar * 1.5f;
        const uint32_t nearCount = out.hasFarCascade ? out.count - 1u : out.count;
        float splitLambda = std::clamp(
            renderer.m_realtimeShadowSettings.cascadeSplitLambda, 0.f, 1.f);
        if (shadowDistance != authoredDistance && nearCount > 1u)
        {
            const float fraction = 1.f / static_cast<float>(nearCount);
            const auto split = [&](float rangeEnd, float lambda) {
                return glm::mix(cameraNear + (rangeEnd - cameraNear) * fraction,
                    cameraNear * std::pow(rangeEnd / cameraNear, fraction), lambda); };
            const float authoredFar = std::min(cameraFar, std::max(cameraNear + 0.001f, authoredDistance));
            const float first = split(authoredFar, splitLambda);
            const float uniform = split(cascadeFar, 0.f), logarithmic = split(cascadeFar, 1.f);
            if (std::abs(uniform - logarithmic) > 1.e-3f)
                splitLambda = std::clamp((uniform - first) / (uniform - logarithmic), 0.f, 1.f);
        }
        for (uint32_t cascade = 0; cascade < nearCount; ++cascade)
        {
            const float fraction = static_cast<float>(cascade + 1u) /
                static_cast<float>(nearCount);
            const float logarithmic = cameraNear * std::pow(
                cascadeFar / cameraNear, fraction);
            const float uniform = cameraNear +
                (cascadeFar - cameraNear) * fraction;
            out.splits[cascade] = glm::mix(uniform, logarithmic, splitLambda);
        }
        if (out.hasFarCascade) out.splits[nearCount] = farDistance;
        const float lastSplit = out.splits[out.count - 1u];
        for (uint32_t cascade = out.count; cascade < 4u; ++cascade)
            out.splits[cascade] = lastSplit;

        const auto cascadePosition = glm::vec3(glm::inverse(cascadeView)[3]);
        const uint32_t tileResolution = out.count > 1u
            ? std::max(1u, atlasResolution / 2u)
            : atlasResolution;
        for (uint32_t cascade = 0; cascade < out.count; ++cascade)
        {
            const bool farCascade = out.hasFarCascade && cascade == out.count - 1u;
            // The far tile keeps its own depth range (its constant depth bias
            // scales with it, matching its texels) and moves in steps of
            // 1/8 of its range, so it is re-rendered only every ~250 m.
            const auto stable = Engine::Rendering::BuildStableShadowCascade(
                cascadePosition,
                shadowSelection.directionToLight,
                out.splits[cascade],
                farCascade ? out.splits[cascade] : shadowDistance,
                tileResolution,
                farCascade || (cascade > 0u && Engine::Rendering::SteppedShadowCascades())
                    ? out.splits[cascade] / 8.f : 0.f);
            out.centers[cascade] = stable.center;
            out.radii[cascade] = stable.radius;
            out.viewProjections[cascade] = stable.viewProjection;
        }
        out.data = {
            static_cast<float>(out.count),
            out.count > 1u ? 0.5f : 1.f,
            std::clamp(renderer.m_realtimeShadowSettings.cascadeTransitionFraction,
                0.f, 0.3f),
            out.splits[out.count - 1u] };
        out.cameraData = glm::vec4(glm::normalize(glm::vec3(
            glm::inverse(cascadeView)[2])), cameraNear);
        return out;
}

SceneRendererShadows::PrepareResult SceneRendererShadows::PrepareDirectionalShadowCascades(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const Engine::Model::RealtimeShadowSelection& shadowSelection,
    const glm::mat4& view,
    const glm::vec3& cameraPosition,
    const Engine::Model::GraphicsQualityValues& quality,
    const Engine::Components::Camera* cam,
    bool shadowOnly)
{
    Scene& scene = renderer.GetScene();
    ShadowCascadeSet mainShadowCascades{};
    bool renderMainDirectionalShadow = shadowSelection.valid;
    std::array<uint8_t, 4> directionalShadowDirtyMask{};
    bool preserveDirectionalShadowAtlas = false;
    bool farRegionResult = false;
    glm::uvec4 farRegionPixels{};
    glm::mat4 farRegionViewProjection(1.f);
    if (shadowSelection.valid)
    {
        ++renderer.m_directionalShadowFrameCounter;
        const ShadowCascadeSet requestedShadowCascades = BuildCascades(
            renderer, shadowSelection, quality, cam, view, renderer.m_directionalShadowResolution,
            renderer.m_realtimeShadowSettings.directionalCascadeCount);
        mainShadowCascades = requestedShadowCascades;
        const bool cacheReusable = renderer.m_directionalShadowCacheValid &&
            renderer.m_directionalShadowCachedQuality.geometry == quality.geometry &&
            renderer.m_directionalShadowCachedQuality.shadows == quality.shadows &&
            renderer.m_directionalShadowCachedQuality.texture == quality.texture &&
            renderer.m_directionalShadowCachedQuality.material == quality.material &&
            renderer.m_directionalShadowCachedResolution ==
                renderer.m_directionalShadowResolution &&
            renderer.m_directionalShadowCachedLightKey ==
                shadowSelection.stableLightKey &&
            renderer.m_directionalShadowCachedCascadeCount ==
                requestedShadowCascades.count &&
            // A shared light direction cannot describe tiles rendered with
            // different orientations. Only tolerate normalization roundoff;
            // sun motion must invalidate every cascade together.
            glm::length(glm::normalize(shadowSelection.directionToLight) -
                renderer.m_directionalShadowCachedDirection) <= 1.e-6f;
        if (shadowOnly)
        {
            bool staggerMovingCascades = false;
            bool farRegionRefresh = false;
            glm::ivec4 farRegionCells{};
            SceneRenderer::FarShadowRegionContent pendingFarRegionContent{};
            auto copyCachedCascades = [&]()
            {
                mainShadowCascades.viewProjections =
                    renderer.m_directionalShadowCachedViewProjections;
                mainShadowCascades.splits =
                    renderer.m_directionalShadowCachedCascadeSplits;
                mainShadowCascades.data =
                    renderer.m_directionalShadowCachedCascadeData;
                mainShadowCascades.cameraData =
                    renderer.m_directionalShadowCachedCameraData;
                mainShadowCascades.centers =
                    renderer.m_directionalShadowCachedCascadeCenters;
                mainShadowCascades.radii =
                    renderer.m_directionalShadowCachedCascadeRadii;
                mainShadowCascades.count =
                    renderer.m_directionalShadowCachedCascadeCount;
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
                        renderer.m_directionalShadowCachedCascadeSplits) > 1e-4f ||
                    glm::length(requestedShadowCascades.data -
                        renderer.m_directionalShadowCachedCascadeData) > 1e-4f ||
                    std::abs(requestedShadowCascades.cameraData.w -
                        renderer.m_directionalShadowCachedCameraData.w) > 1e-4f;
                // Rotation and lens changes do not alter radial coverage.
                // Stagger walking updates only while every retained tile can
                // still cover its entire current receiver sphere.
                staggerMovingCascades = !globalCascadeDataChanged &&
                    glm::length(cameraPosition-renderer.m_directionalShadowCachedCameraPosition)<=1.f;
                for(uint32_t cascade=0;cascade<requestedShadowCascades.count;++cascade)
                    if(glm::length(cameraPosition-renderer.m_directionalShadowCachedCascadeCenters[cascade])+
                        requestedShadowCascades.splits[cascade]>
                        renderer.m_directionalShadowCachedCascadeRadii[cascade]-.01f)
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
                            renderer.m_directionalShadowCachedCascadeCenters[cascade]);
                        const float radiusDelta = std::abs(
                            requestedShadowCascades.radii[cascade] -
                            renderer.m_directionalShadowCachedCascadeRadii[cascade]);
                        // Range/split changes alter radial coverage; refresh
                        // the layout together rather than staggering it.
                        if(radiusDelta>0.05f) staggerMovingCascades=false;
                        const float projectionDelta = matrixDelta(
                            requestedShadowCascades.viewProjections[cascade],
                            renderer.m_directionalShadowCachedViewProjections[cascade]);
                        directionalShadowDirtyMask[cascade] =
                            (centerDelta > 0.05f || radiusDelta > 0.05f ||
                                projectionDelta > 1e-4f) ? 1u : 0u;
                    }
                    // The far tile is not in the fast rotation: it holds
                    // terrain and distant trees, and costs several ms of
                    // CPU to draw. Besides its stepped center or the sun
                    // moving, it refreshes when objects have streamed in or
                    // out (at most every 240 shadow frames: streaming changes
                    // the scene almost every frame while flying), or every 600.
                    // Stepped cascades hold plants in their rest pose: only
                    // the nearest is refreshed for wind.
                    const uint32_t refreshCount = Engine::Rendering::SteppedShadowCascades()
                        ? 1u
                        : requestedShadowCascades.hasFarCascade
                        ? requestedShadowCascades.count - 1u
                        : requestedShadowCascades.count;
                    const uint32_t refreshCascade =
                        renderer.m_directionalShadowRefreshCursor % refreshCount;
                    const bool temporalRefreshDue =
                        (renderer.m_directionalShadowFrameCounter -
                            renderer.m_directionalShadowCascadeLastRenderedFrame[refreshCascade]) >= 2u;
                    if (temporalRefreshDue)
                    {
                        directionalShadowDirtyMask[refreshCascade] = 1u;
                        renderer.m_directionalShadowRefreshCursor =
                            (refreshCascade + 1u) % refreshCount;
                    }
                    if (requestedShadowCascades.hasFarCascade)
                    {
                        const uint32_t farCascade = requestedShadowCascades.count - 1u;
                        const uint64_t age = renderer.m_directionalShadowFrameCounter -
                            renderer.m_directionalShadowCascadeLastRenderedFrame[farCascade];
                        const bool contentChanged = scene.GetStructureRevision() !=
                            renderer.m_directionalShadowFarStructureRevision;
                        if (directionalShadowDirtyMask[farCascade]) {}
                        else if (age >= 600u ||
                            (contentChanged && age >= 240u && !FarShadowRegionRefresh()))
                            directionalShadowDirtyMask[farCascade] = 1u;
                        // Casters that streamed in or out (a far tree block, a
                        // near/far hand-off) are drawn within a few frames by
                        // redrawing only the regions of the tile whose casters
                        // changed, instead of the whole tile 4-10 s later (far
                        // trees showed up without shadows, which then appeared
                        // at once). Checked every 4 shadow frames; at most a
                        // quarter of the tile per refresh, nearest first.
                        else if (FarShadowRegionRefresh() &&
                            renderer.m_directionalShadowFarRegionContentValid &&
                            context->SupportsDepthRectangleClear() &&
                            renderer.m_directionalShadowFrameCounter -
                                renderer.m_directionalShadowFarRegionCheckFrame >= 4u)
                        {
                            renderer.m_directionalShadowFarRegionCheckFrame =
                                renderer.m_directionalShadowFrameCounter;
                            const glm::mat4& farViewProjection =
                                renderer.m_directionalShadowCachedViewProjections[farCascade];
                            pendingFarRegionContent = ShadowCascadeRegionContent(
                                renderer.m_frameRenderItems, farViewProjection);
                            const glm::vec4 cameraClip = farViewProjection *
                                glm::vec4(cameraPosition, 1.f);
                            constexpr float grid = float(SceneRenderer::kFarShadowRegionGrid);
                            const glm::vec2 cameraCell((cameraClip.x * .5f + .5f) * grid,
                                (.5f - cameraClip.y * .5f) * grid);
                            constexpr uint32_t maxCells = SceneRenderer::kFarShadowRegionGrid *
                                SceneRenderer::kFarShadowRegionGrid / 4u;
                            if (ChooseFarShadowRegions(pendingFarRegionContent,
                                    renderer.m_directionalShadowFarRegionContent,
                                    cameraCell, maxCells, farRegionCells))
                            {
                                farRegionRefresh = true;
                                directionalShadowDirtyMask[farCascade] = 1u;
                            }
                        }
                    }
                    // Stepped middle cascades refresh only when the camera
                    // crosses a step, so casters that streamed in or out (or a
                    // near/far block hand-off) since kept their old shadows,
                    // and the shadows then changed at once at the next step
                    // (a lit bush and rock went dark in one frame). One
                    // cascade's content is checked per frame, in turn, and a
                    // changed one refreshes (at most every 8 frames each, at
                    // least every 120).
                    // ENGINE_SHADOW_CONTENT_REFRESH=0 turns the check off (A/B runs).
                    static const bool contentRefresh = [] {
                        const char* value = std::getenv("ENGINE_SHADOW_CONTENT_REFRESH");
                        return !(value && value[0] == '0');
                    }();
                    if (Engine::Rendering::SteppedShadowCascades() && contentRefresh)
                    {
                        const uint32_t middle = requestedShadowCascades.count -
                            (requestedShadowCascades.hasFarCascade ? 1u : 0u);
                        if (middle > 1u)
                        {
                            const uint32_t cascade = 1u + renderer.m_directionalShadowContentCursor++ % (middle - 1u);
                            const uint64_t age = renderer.m_directionalShadowFrameCounter -
                                renderer.m_directionalShadowCascadeLastRenderedFrame[cascade];
                            // Also every 120 frames: a cached image can go stale
                            // without its casters changing (drawn while tree
                            // levels and leaf textures were still streaming in,
                            // which nothing announces; a still camera never
                            // crosses a step), and stayed so until the next step.
                            if (!directionalShadowDirtyMask[cascade] && age >= 8u &&
                                (age >= 120u || ShadowCascadeContent(renderer.m_frameRenderItems,
                                    renderer.m_directionalShadowCachedViewProjections[cascade]) !=
                                    renderer.m_directionalShadowCascadeContent[cascade]))
                                directionalShadowDirtyMask[cascade] = 1u;
                        }
                    }
                }
            }
            if (staggerMovingCascades && requestedShadowCascades.count>2u)
            {
                // At most two tiles per frame: prioritize a dirty near tile,
                // then visit far tiles in a rotating order without starvation.
                uint32_t remaining=2u-(directionalShadowDirtyMask[0] ? 1u:0u);
                const uint32_t farCount=requestedShadowCascades.count-1u;
                const uint32_t first=uint32_t(renderer.m_directionalShadowFrameCounter%farCount);
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
            // The content each re-rendered middle cascade is drawn with
            // (in its new light-space square), for the check above.
            if (Engine::Rendering::SteppedShadowCascades())
                for (uint32_t cascade = 1u; cascade < requestedShadowCascades.count -
                        (requestedShadowCascades.hasFarCascade ? 1u : 0u); ++cascade)
                    if (directionalShadowDirtyMask[cascade])
                        renderer.m_directionalShadowCascadeContent[cascade] = ShadowCascadeContent(
                            renderer.m_frameRenderItems, requestedShadowCascades.viewProjections[cascade]);
            // The far cascade's region signatures: all of them after a whole
            // refresh (in its new light-space square), those inside the
            // rectangle after a region refresh (the others wait their turn).
            if (requestedShadowCascades.hasFarCascade)
            {
                const uint32_t farCascade = requestedShadowCascades.count - 1u;
                if (!directionalShadowDirtyMask[farCascade])
                    farRegionRefresh = false;
                else if (farRegionRefresh)
                {
                    constexpr int grid = int(SceneRenderer::kFarShadowRegionGrid);
                    for (int r = farRegionCells.y; r <= farRegionCells.w; ++r)
                        for (int c = farRegionCells.x; c <= farRegionCells.z; ++c)
                            renderer.m_directionalShadowFarRegionContent[size_t(r * grid + c)] =
                                pendingFarRegionContent[size_t(r * grid + c)];
                    const uint32_t tileResolution = requestedShadowCascades.count > 1u
                        ? std::max(1u, renderer.m_directionalShadowResolution / 2u)
                        : renderer.m_directionalShadowResolution;
                    const auto pixel = [&](int cell)
                    { return uint32_t(uint64_t(cell) * tileResolution / uint32_t(grid)); };
                    farRegionPixels = { pixel(farRegionCells.x), pixel(farRegionCells.y),
                        pixel(farRegionCells.z + 1), pixel(farRegionCells.w + 1) };
                    // Same texels as the whole tile: the cached projection
                    // followed by a clip-space crop onto the rectangle, which
                    // the viewport then maps back onto the same atlas texels
                    // (row 0 at the top: v = 0.5 - 0.5 y on every backend).
                    const float size = float(tileResolution);
                    const float x0 = farRegionPixels.x / size * 2.f - 1.f;
                    const float x1 = farRegionPixels.z / size * 2.f - 1.f;
                    const float yTop = 1.f - farRegionPixels.y / size * 2.f;
                    const float yBottom = 1.f - farRegionPixels.w / size * 2.f;
                    glm::mat4 crop(1.f);
                    crop[0][0] = 2.f / (x1 - x0);
                    crop[1][1] = 2.f / (yTop - yBottom);
                    crop[3][0] = -(x0 + x1) / (x1 - x0);
                    crop[3][1] = -(yTop + yBottom) / (yTop - yBottom);
                    farRegionViewProjection = crop *
                        renderer.m_directionalShadowCachedViewProjections[farCascade];
                    renderer.m_lastShadowCpuTiming.farRegionCells = uint32_t(
                        (farRegionCells.z - farRegionCells.x + 1) * (farRegionCells.w - farRegionCells.y + 1));
                }
                else
                {
                    renderer.m_directionalShadowFarRegionContent = ShadowCascadeRegionContent(
                        renderer.m_frameRenderItems, requestedShadowCascades.viewProjections[farCascade]);
                    renderer.m_directionalShadowFarRegionContentValid = true;
                }
            }
            // Refresh blend: advance the fades in progress, then send each
            // redrawn cached cascade (past the nearest) to a spare tile and
            // fade its receivers over from the image it replaces.
            const bool blendAtlas = Engine::Rendering::ShadowRefreshBlend() &&
                renderer.m_directionalShadowAtlasRows == 3u &&
                requestedShadowCascades.count > 1u &&
                context->SupportsDepthRectangleClear();
            auto& tiles = renderer.m_directionalShadowCascadeTile;
            auto& blends = renderer.m_directionalShadowBlends;
            ++renderer.m_directionalShadowBlendFrame;
            if (!blendAtlas || !cacheReusable)
            {
                tiles = { 0u, 1u, 2u, 3u };
                for (auto& blend : blends) blend = {};
            }
            else
            {
                const float blendFrames = float(Engine::Rendering::ShadowRefreshBlendFrames());
                for (auto& blend : blends)
                    if (blend.active)
                    {
                        blend.weight = float(renderer.m_directionalShadowBlendFrame -
                            blend.startFrame + 1u) / blendFrames;
                        if (blend.weight >= 1.f) blend = {};
                    }
                for (uint32_t cascade = 1u; cascade < requestedShadowCascades.count; ++cascade)
                {
                    if (!directionalShadowDirtyMask[cascade]) continue;
                    const bool farTile = requestedShadowCascades.hasFarCascade &&
                        cascade + 1u == requestedShadowCascades.count;
                    // A region refresh draws into the tile it keeps.
                    if (farTile && farRegionRefresh) continue;
                    // Unstepped middle cascades follow the camera texel by
                    // texel; there is no step to hide.
                    if (!farTile && !Engine::Rendering::SteppedShadowCascades()) continue;
                    // A fade of this cascade still in progress ends now: its
                    // current image becomes the one faded from.
                    for (auto& blend : blends)
                        if (blend.active && blend.cascade == cascade) blend = {};
                    const glm::mat4& previous =
                        renderer.m_directionalShadowCachedViewProjections[cascade];
                    const glm::mat4& next = requestedShadowCascades.viewProjections[cascade];
                    // The previous image is reached through the new
                    // projection plus a clip-space offset: only while the
                    // square moved without turning or scaling (stepped).
                    bool translated = true;
                    for (int column = 0; column < 3; ++column)
                        for (int row = 0; row < 4; ++row)
                            if (std::abs(previous[column][row] - next[column][row]) >
                                1.e-5f * std::max(1.f, std::abs(next[column][row])))
                                translated = false;
                    if (!translated) continue;
                    auto* slot = &blends[0];
                    for (auto& blend : blends)
                        if (!blend.active) { slot = &blend; break; }
                    if (slot->active)
                    {
                        // Both slots busy: end the fade nearest its end.
                        slot = blends[0].weight >= blends[1].weight ? &blends[0] : &blends[1];
                        *slot = {};
                    }
                    std::array<bool, 6> used{};
                    for (uint32_t other = 0; other < requestedShadowCascades.count; ++other)
                        used[tiles[other]] = true;
                    for (const auto& blend : blends)
                        if (blend.active) used[blend.previousTile] = true;
                    uint32_t spare = 0;
                    while (spare < 6u && used[spare]) ++spare;
                    if (spare >= 6u) continue;
                    slot->active = true;
                    slot->cascade = cascade;
                    slot->previousTile = tiles[cascade];
                    slot->startFrame = renderer.m_directionalShadowBlendFrame;
                    slot->clipOffset = glm::vec3(previous[3] - next[3]);
                    slot->weight = 1.f / blendFrames;
                    tiles[cascade] = spare;
                }
            }
            if (!renderMainDirectionalShadow && cacheReusable)
                copyCachedCascades();
            // With the blend, the tiles not redrawn (and the images being
            // faded from) must survive: clear only the tiles drawn.
            preserveDirectionalShadowAtlas = cacheReusable &&
                renderMainDirectionalShadow &&
                (dirtyCascadeCount < requestedShadowCascades.count || farRegionRefresh ||
                    blendAtlas);
            if (preserveDirectionalShadowAtlas)
                for (uint32_t cascade = 0; cascade < requestedShadowCascades.count; ++cascade)
                    if (!directionalShadowDirtyMask[cascade])
                    {
                        mainShadowCascades.viewProjections[cascade] =
                            renderer.m_directionalShadowCachedViewProjections[cascade];
                        mainShadowCascades.centers[cascade] =
                            renderer.m_directionalShadowCachedCascadeCenters[cascade];
                        mainShadowCascades.radii[cascade] =
                            renderer.m_directionalShadowCachedCascadeRadii[cascade];
                    }
            if (farRegionRefresh)
            {
                // The tile itself keeps its cached square; only the drawing
                // uses the cropped projection (PrepareResult).
                const uint32_t farCascade = requestedShadowCascades.count - 1u;
                mainShadowCascades.viewProjections[farCascade] =
                    renderer.m_directionalShadowCachedViewProjections[farCascade];
                mainShadowCascades.centers[farCascade] =
                    renderer.m_directionalShadowCachedCascadeCenters[farCascade];
                mainShadowCascades.radii[farCascade] =
                    renderer.m_directionalShadowCachedCascadeRadii[farCascade];
            }
            farRegionResult = farRegionRefresh;
        }
        else if (renderer.m_directionalShadowCacheValid)
        {
            mainShadowCascades.viewProjections =
                renderer.m_directionalShadowCachedViewProjections;
            mainShadowCascades.splits =
                renderer.m_directionalShadowCachedCascadeSplits;
            mainShadowCascades.data =
                renderer.m_directionalShadowCachedCascadeData;
            mainShadowCascades.cameraData =
                renderer.m_directionalShadowCachedCameraData;
            mainShadowCascades.centers =
                renderer.m_directionalShadowCachedCascadeCenters;
            mainShadowCascades.radii =
                renderer.m_directionalShadowCachedCascadeRadii;
            mainShadowCascades.count =
                renderer.m_directionalShadowCachedCascadeCount;
        }
    }
    {
        const auto& tiles = renderer.m_directionalShadowCascadeTile;
        mainShadowCascades.atlasTiles = glm::vec4(
            float(tiles[0] | tiles[1] << 3u | tiles[2] << 6u | tiles[3] << 9u), -1.f, -1.f,
            float(renderer.m_directionalShadowAtlasRows));
        for (uint32_t slot = 0; slot < 2u; ++slot)
        {
            const auto& blend = renderer.m_directionalShadowBlends[slot];
            if (!blend.active) continue;
            mainShadowCascades.atlasTiles[int(slot) + 1] =
                float(blend.cascade + 8u * blend.previousTile);
            mainShadowCascades.refreshBlends[slot] = glm::vec4(blend.clipOffset, blend.weight);
        }
    }
    PrepareResult result{};
    result.cascades = mainShadowCascades;
    result.dirtyMask = directionalShadowDirtyMask;
    result.renderMainDirectionalShadow = renderMainDirectionalShadow;
    result.preserveDirectionalShadowAtlas = preserveDirectionalShadowAtlas;
    result.farRegionRefresh = farRegionResult;
    result.farRegionPixels = farRegionPixels;
    result.farRegionViewProjection = farRegionViewProjection;
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
