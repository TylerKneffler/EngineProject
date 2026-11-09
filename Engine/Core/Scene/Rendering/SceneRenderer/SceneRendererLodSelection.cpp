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
    bool includeEditorVisuals)
{
    m_scene = &scene;
    m_draws = &draws;
    m_camera = camera;
    m_instanceViewOrdinal = instanceViewOrdinal;
    m_selectedLods.assign(draws.size(), 1);
    Engine::Rendering::RenderJobs::Get().For(draws.size(), 512,
        [&](size_t index)
        {
            const auto& draw = draws[index];
            if (draw.gpuLod < 0 || draw.mesh->prefabLodBand >= 0)
                return;
            const glm::vec2 distances = SceneInstanceBatching::
                ScaleShadowLodDistances(draw.gpuLodDistances, shadowOnly);
            const float distance = glm::length(
                glm::vec3(draw.objectData.world[3]) - cameraPosition);
            const int lod = distance < distances.x ? 0
                : distance < distances.y ? 1 : 2;
            if (draw.mesh->IsInstanced())
            {
                const float width = std::max(.001f,
                    std::min(.5f, (distances.y - distances.x) * .25f));
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

    if (shadowOnly)
        return;
    const auto quality = scene.settings.graphicsQuality.Resolve();
    for (size_t index = 0; index < draws.size(); ++index)
    {
        const auto& draw = draws[index];
        auto* policy = SceneRendererDrawPreparation::ScreenLodPolicy(
            scene, draw, hasPortalViews, includeEditorVisuals);
        if (!policy || draw.mesh->IsInstanced())
            continue;
        glm::vec3 minimum = policy->GetBoundsMin();
        glm::vec3 maximum = policy->GetBoundsMax();
        const float wind = std::clamp(policy->foliageWindStrength, 0.f, 1.f);
        for (int level = 0; level <= draw.gpuMaximumLod; ++level)
        {
            auto* geometry = policy->GetRuntimeLodMesh(level);
            if (!geometry || !geometry->HasBounds())
                continue;
            const glm::vec3 padding(
                1.5f * std::max(0.f, geometry->GetBoundsMax().y) * wind);
            minimum = glm::min(minimum, geometry->GetBoundsMin() - padding);
            maximum = glm::max(maximum, geometry->GetBoundsMax() + padding);
        }
        const int selected = ChooseScreen(*policy, minimum, maximum,
            draw.objectData.world, scene.GetObjectId(draw.object), 0, 0,
            draw.gpuMaximumLod);
        m_selectedLods[index] = draw.gpuLod == selected;
        if (draw.gpuLod == 0)
            ++scene.m_screenLodCounts[selected];
    }
}

bool SceneRendererLodSelection::IsSelected(
    const PreparedDraw& draw, bool shadow) const
{
    const auto& selected = shadow ? m_distanceSelectedLods : m_selectedLods;
    return selected[size_t(&draw - m_draws->data())] != 0;
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
