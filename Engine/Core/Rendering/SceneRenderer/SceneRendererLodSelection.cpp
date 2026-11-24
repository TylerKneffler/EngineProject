#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererLodSelection.h"

#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Rendering/ScreenCoverage.h"
#include "Core/Rendering/DetailBudget.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/SceneRenderer/SceneInstanceBatching.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
void SceneRendererLodSelection::Prepare(
    SceneRenderer& renderer,
    const std::vector<PreparedDraw>& draws,
    const Engine::Components::Camera* camera,
    const glm::vec3& cameraPosition,
    const glm::mat4& viewProjection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    uint64_t instanceViewOrdinal,
    bool shadowOnly,
    bool hasPortalViews,
    bool includeEditorVisuals,
    int instanceLodDisplayOverride)
{
    Scene& scene = renderer.GetScene();
    m_renderer = &renderer;
    m_draws = &draws;
    m_camera = camera;
    m_instanceViewOrdinal = instanceViewOrdinal;
    m_selectedLods.assign(draws.size(), 1);
    const auto isBeyondFinalLod = [&](const PreparedDraw& draw)
    {
        if (draw.mesh->IsInstanced()) return false;
        const auto* policy = draw.object
            ? draw.object->GetComponent<Engine::Components::Mesh>() : draw.mesh;
        if (!policy || !policy->cullAfterLastLod ||
            policy->finalLodCullDistance <= 0.f || !std::isfinite(policy->finalLodCullDistance))
            return false;
        glm::vec3 minimum, maximum;
        if (!policy->GetRuntimeLodAggregateBounds(minimum, maximum))
            return false;
        const glm::vec3 localCenter = (minimum + maximum) * .5f;
        const glm::vec3 localExtent = (maximum - minimum) * .5f;
        const glm::vec3 center(draw.objectData.world * glm::vec4(localCenter, 1.f));
        const glm::vec3 axisLengths(
            glm::length(glm::vec3(draw.objectData.world[0])),
            glm::length(glm::vec3(draw.objectData.world[1])),
            glm::length(glm::vec3(draw.objectData.world[2])));
        const float radius = glm::length(localExtent * axisLengths);
        // Scaled like every other LOD distance of the draw (quality, detail
        // budget, screen coverage), matching the GPU cull's final range.
        return std::max(0.f, glm::length(center - cameraPosition) - radius) >=
            policy->finalLodCullDistance * draw.lodScale;
    };
    Engine::Rendering::RenderJobs::Get().For(draws.size(), 512,
        [&](size_t index)
        {
            const auto& draw = draws[index];
            const bool forceDisplayLod = !shadowOnly && includeEditorVisuals &&
                instanceLodDisplayOverride >= 0;
            if (!forceDisplayLod && isBeyondFinalLod(draw))
            {
                m_selectedLods[index] = 0;
                return;
            }
            if (forceDisplayLod)
            {
                int maximum = 0;
                if (draw.mesh->instanceLodBand >= 0)
                {
                    if (const auto data = draw.mesh->GetInstanceData())
                        maximum = std::max(0, data->maximumLod);
                }
                else
                    maximum = std::max(0, draw.gpuMaximumLod);
                const int selected = std::min(instanceLodDisplayOverride, maximum);
                m_selectedLods[index] = draw.mesh->instanceLodBand >= 0
                    ? draw.mesh->instanceLodBand == selected
                    : (draw.gpuLod < 0 || draw.gpuLod == selected);
                return;
            }
            if (draw.gpuLod < 0 || draw.mesh->instanceLodBand >= 0)
                return;
            const glm::vec2 distances = SceneInstanceBatching::
                ScaleShadowLodDistances(draw.gpuLodDistances, shadowOnly);
            const float distance = glm::length(
                glm::vec3(draw.objectData.world[3]) - cameraPosition);
            const int lod = distance < distances.x ? 0
                : distance < distances.y ? 1 : 2;
            if (draw.mesh->IsInstanced())
            {
                const float width = std::clamp(draw.mesh->lodTransitionWidth,
                    0.f, .5f) * std::max(0.f, distances.y - distances.x);
                m_selectedLods[index] = draw.gpuLod == 0
                    ? distance <= distances.x + width
                    : draw.gpuLod == 1
                        ? distance >= distances.x - width &&
                            distance <= distances.y + width
                        : distance >= distances.y - width;
            }
            else
                m_selectedLods[index] =
                    std::min(lod, draw.gpuMaximumLod) == draw.gpuLod;
        });
    m_distanceSelectedLods = m_selectedLods;

    m_screenSerial = renderer.m_graphicsProvider->GetContextFactory()->
        GetPreparedFrameSerial();
    for (auto it = renderer.m_screenLodHistory.begin();
        it != renderer.m_screenLodHistory.end();)
    {
        if (m_screenSerial > it->second.second &&
            m_screenSerial - it->second.second > 240)
            it = renderer.m_screenLodHistory.erase(it);
        else
            ++it;
    }
    m_screenView = { viewProjection,
        { static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) },
        true };

    if (shadowOnly || (includeEditorVisuals &&
        instanceLodDisplayOverride >= 0))
        return;
    const auto quality = scene.settings.graphicsQuality.Resolve();
    for (size_t index = 0; index < draws.size(); ++index)
    {
        const auto& draw = draws[index];
        if (isBeyondFinalLod(draw)) continue;
        auto* policy = SceneRendererDrawPreparation::ScreenLodPolicy(renderer, draw, hasPortalViews, includeEditorVisuals);
        if (!policy || draw.mesh->IsInstanced())
            continue;
        glm::vec3 minimum, maximum;
        if (!policy->GetRuntimeLodAggregateBounds(minimum, maximum))
            continue;
        const int selected = ChooseScreen(*policy, minimum, maximum,
            draw.objectData.world, scene.GetObjectId(draw.object), 0, 0,
            draw.gpuMaximumLod, Engine::Rendering::DetailBudget::Get().LayerLodScale(
                draw.object ? draw.object->layer : 0));
        m_selectedLods[index] = draw.gpuLod == selected;
        if (draw.gpuLod == 0)
        {
            ++renderer.m_screenLodCounts[selected];
            ++renderer.m_lastRenderCpuTimings.cameraVisibility
                .screenSelectedInstancesByLod[selected];
            const auto* geometry = policy->GetRuntimeLodMesh(selected);
            if (geometry)
                renderer.m_lastRenderCpuTimings.cameraVisibility
                    .screenSelectedTrianglesByLod[selected] +=
                    (geometry->GetIndexBuffer() ? geometry->GetIndexCount()
                        : geometry->GetVertexCount()) / 3u;
        }
    }
}

bool SceneRendererLodSelection::IsSelected(
    const PreparedDraw& draw, bool shadow) const
{
    const auto& selected = shadow ? m_distanceSelectedLods : m_selectedLods;
    return selected[size_t(&draw - m_draws->data())] != 0;
}

std::array<uint64_t, 3> SceneRendererLodSelection::SelectedGpuLodDraws(
    bool shadow) const
{
    std::array<uint64_t, 3> counts{};
    if (!m_draws) return counts;
    const auto& selected = shadow ? m_distanceSelectedLods : m_selectedLods;
    const size_t count = std::min(m_draws->size(), selected.size());
    for (size_t index = 0; index < count; ++index)
    {
        const int lod = (*m_draws)[index].gpuLod;
        if (selected[index] && lod >= 0 && lod < static_cast<int>(counts.size()))
            ++counts[static_cast<size_t>(lod)];
    }
    return counts;
}

int SceneRendererLodSelection::ChooseScreen(
    const Engine::Components::Mesh& policy,
    glm::vec3 minimum,
    glm::vec3 maximum,
    const glm::mat4& world,
    uint64_t identity,
    uint64_t placement,
    uint64_t domain,
    int maximumLod,
    float detailScale)
{
    const std::array<uint64_t, 4> key{
        uint64_t(reinterpret_cast<uintptr_t>(m_camera)), identity, placement,
        (m_instanceViewOrdinal << 32) | domain };
    auto [entry, inserted] = m_renderer->m_screenLodHistory.try_emplace(
        key, std::make_pair(-1, m_screenSerial));
    std::vector<float> thresholds{ policy.screenLod1Pixels };
    if (maximumLod > 1)
        thresholds.push_back(policy.screenLod2Pixels);
    const auto quality = m_renderer->GetScene().settings.graphicsQuality.Resolve();
    float pixels = 0.f;
    if (Engine::Rendering::FairVisibilityLod() && m_camera)
    {
        // Fair visibility: the size the reference camera would see at this
        // world distance, the same for any zoom, resolution, view angle,
        // quality setting or detail budget (ScreenCoverage.h).
        const auto bounds = Engine::Rendering::TransformBounds(minimum, maximum, world);
        const glm::vec3 centre = (bounds.minimum + bounds.maximum) * .5f;
        const float diameter = glm::length(bounds.maximum - bounds.minimum);
        const glm::vec3 eye(m_camera->GetWorldPose().eye);
        pixels = Engine::Rendering::ReferenceLodPixels(diameter, glm::length(centre - eye));
    }
    else
        pixels = Engine::Rendering::ProjectedLodPixels(
            minimum, maximum, world, m_screenView) * quality.geometry *
            (std::isfinite(detailScale) && detailScale > 0.f ? detailScale : 1.f);
    const int selected = Engine::Rendering::SelectScreenMeshLod(pixels,
        thresholds, entry->second.first, policy.screenLodHysteresis);
    entry->second = { std::min(selected, maximumLod), m_screenSerial };
    return entry->second.first;
}

std::map<uint64_t, std::vector<int>>&
SceneRendererLodSelection::PlacementLevels()
{
    return m_screenPlacementLevels;
}
}
