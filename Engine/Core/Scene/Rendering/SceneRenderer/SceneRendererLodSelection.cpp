#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLodSelection.h"

#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneInstanceBatching.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Engine::Scene::Renderer
{
void SceneRendererLodSelection::Prepare(
    Scene& scene,
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
    m_scene = &scene;
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
        return std::max(0.f, glm::length(center - cameraPosition) - radius) >=
            policy->finalLodCullDistance;
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

    m_screenSerial = scene.m_graphicsProvider->GetContextFactory()->
        GetPreparedFrameSerial();
    for (auto it = scene.m_screenLodHistory.begin();
        it != scene.m_screenLodHistory.end();)
    {
        if (m_screenSerial > it->second.second &&
            m_screenSerial - it->second.second > 240)
            it = scene.m_screenLodHistory.erase(it);
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
        auto* policy = SceneRendererDrawPreparation::ScreenLodPolicy(
            scene, draw, hasPortalViews, includeEditorVisuals);
        if (!policy || draw.mesh->IsInstanced())
            continue;
        glm::vec3 minimum, maximum;
        if (!policy->GetRuntimeLodAggregateBounds(minimum, maximum))
            continue;
        const int selected = ChooseScreen(*policy, minimum, maximum,
            draw.objectData.world, scene.GetObjectId(draw.object), 0, 0,
            draw.gpuMaximumLod);
        m_selectedLods[index] = draw.gpuLod == selected;
        if (draw.gpuLod == 0)
        {
            ++scene.m_screenLodCounts[selected];
            ++scene.m_lastRenderCpuTimings.cameraVisibility
                .screenSelectedInstancesByLod[selected];
            const auto* geometry = policy->GetRuntimeLodMesh(selected);
            if (geometry)
                scene.m_lastRenderCpuTimings.cameraVisibility
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
    int maximumLod)
{
    const std::array<uint64_t, 4> key{
        uint64_t(reinterpret_cast<uintptr_t>(m_camera)), identity, placement,
        (m_instanceViewOrdinal << 32) | domain };
    auto [entry, inserted] = m_scene->m_screenLodHistory.try_emplace(
        key, std::make_pair(-1, m_screenSerial));
    std::vector<float> thresholds{ policy.screenLod1Pixels };
    if (maximumLod > 1)
        thresholds.push_back(policy.screenLod2Pixels);
    const auto quality = m_scene->settings.graphicsQuality.Resolve();
    const float pixels = Engine::Rendering::ProjectedLodPixels(
        minimum, maximum, world, m_screenView) * quality.geometry;
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
