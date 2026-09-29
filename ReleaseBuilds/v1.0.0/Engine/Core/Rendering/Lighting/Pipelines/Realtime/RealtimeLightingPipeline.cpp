#include "RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <optional>
#include <vector>

namespace Engine::Rendering
{
    uint32_t RealtimeLightingPipeline::CollectLights(
        const Engine::Scene::Scene& scene,
        LightData* destination,
        uint32_t capacity,
        bool mapThroughSpatialVolumes,
        const Engine::Model::RealtimeShadowSettings* shadowSettings,
        Engine::Model::RealtimeShadowSelection* shadowSelection,
        const glm::vec3* importancePosition) const
    {
        if (shadowSelection)
            *shadowSelection = {};
        if (!destination || capacity == 0)
            return 0;

        struct ShadowCandidate
        {
            uint32_t lightIndex = 0;
            float score = 0.f;
            std::string stableKey;
            glm::vec3 directionToLight{ 0.f, 1.f, 0.f };
            float depthBias = 0.f;
            float normalBias = 0.f;
            float strength = 0.f;
            float resolutionScale = 1.f;
            float filterScale = 1.f;
        };
        std::optional<ShadowCandidate> selectedShadow;
        const auto stableObjectPath = [](const Engine::Core::Object* object)
        {
            std::string path;
            for (const Engine::Core::Object* current = object;
                current; current = current->Parent)
            {
                path.insert(0, "/" + current->name);
            }
            return path;
        };

        struct LightCandidate
        {
            LightData data{};
            const Engine::Components::Light* light = nullptr;
            std::string stableKey;
            float importance = 0.f;
        };
        std::vector<LightCandidate> candidates;
        const auto importanceScore = [&](const LightData& data,
            const Engine::Components::Light& light)
        {
            const float radiance = std::max(0.f, light.intensity) *
                std::max({ 0.f, light.color.r, light.color.g, light.color.b });
            if (light.GetLightType() == Engine::Components::Light::Type::Directional)
                return radiance * 1000000.f;
            if (!importancePosition || light.range <= 0.f)
                return radiance * std::max(0.f, light.range);
            const float distance = glm::distance(
                glm::vec3(data.positionRange), *importancePosition);
            const float normalized = std::clamp(
                1.f - distance / light.range, 0.f, 1.f);
            return radiance * normalized * normalized;
        };
        for (const auto& candidate : scene.GetObjects())
        {
            if (!candidate->IsEnabledInHierarchy())
                continue;
            const Engine::Components::Light* light =
                candidate->GetComponent<Engine::Components::Light>();
            if (!light || light->baked || light->intensity <= 0.f)
                continue;
            if (light->GetLightType() == Engine::Components::Light::Type::Point && light->range <= 0.f)
                continue;

            glm::mat4 world = candidate->transform.GetWorldMatrix();
            if (candidate->transform.matrixLayer.enabled)
                world = candidate->transform.matrixLayer.localToLayer * world;
            if (mapThroughSpatialVolumes)
            {
                world = scene.MapSpatialMatrix(world,
                    { Engine::Scene::Scene::SpatialQueryDomain::Rendering,
                        candidate.get() });
            }
            LightData data{};
            if (light->GetLightType() == Engine::Components::Light::Type::Directional)
            {
                const glm::vec3 rayDirection = glm::normalize(glm::vec3(world[2]));
                data.positionRange = glm::vec4(-rayDirection, 0.f);
            }
            else
            {
                data.positionRange = glm::vec4(
                    glm::vec3(world[3]), light->range);
            }
            data.colorIntensity = glm::vec4(light->color, light->intensity);
            const bool directional = light->GetLightType() ==
                Engine::Components::Light::Type::Directional;
            data.params = glm::vec4(light->falloff, directional ? 1.f : 0.f,
                -1.f, 0.f);
            const std::string stableKey = stableObjectPath(candidate.get());
            candidates.push_back({ data, light, stableKey,
                importanceScore(data, *light) });

            const Engine::Components::MatrixLayerConnection& connection =
                candidate->transform.matrixLayer.connection;
            if (!connection.enabled)
                continue;

            LightData mapped = data;
            if (light->GetLightType() == Engine::Components::Light::Type::Directional)
            {
                const glm::vec3 sourceDirection = -glm::vec3(data.positionRange);
                const glm::vec3 mappedDirection = glm::normalize(glm::vec3(
                    connection.localToRemote * glm::vec4(sourceDirection, 0.f)));
                mapped.positionRange = glm::vec4(-mappedDirection, 0.f);
            }
            else
            {
                const glm::vec3 sourcePosition = glm::vec3(data.positionRange);
                mapped.positionRange = glm::vec4(
                    connection.TransformPoint(sourcePosition), light->range);
            }

            candidates.push_back({ mapped, light, stableKey + "/mapped",
                importanceScore(mapped, *light) });
        }

        std::stable_sort(candidates.begin(), candidates.end(),
            [](const LightCandidate& left, const LightCandidate& right)
            {
                constexpr float epsilon = 0.000001f;
                if (std::abs(left.importance - right.importance) > epsilon)
                    return left.importance > right.importance;
                return left.stableKey < right.stableKey;
            });
        const uint32_t count = std::min(capacity,
            static_cast<uint32_t>(candidates.size()));
        for (uint32_t index = 0; index < count; ++index)
        {
            destination[index] = candidates[index].data;
            const Engine::Components::Light& light = *candidates[index].light;
            const bool directional = light.GetLightType() ==
                Engine::Components::Light::Type::Directional;
            if (!directional || !light.castsShadows || !shadowSettings ||
                !shadowSettings->enabled ||
                shadowSettings->maximumShadowedLights == 0u || !shadowSelection)
                continue;
            ShadowCandidate shadowCandidate{};
            shadowCandidate.lightIndex = index;
            shadowCandidate.strength = std::clamp(light.shadowStrength, 0.f, 1.f);
            shadowCandidate.score = candidates[index].importance *
                shadowCandidate.strength;
            shadowCandidate.stableKey = candidates[index].stableKey;
            shadowCandidate.directionToLight = glm::normalize(
                glm::vec3(candidates[index].data.positionRange));
            shadowCandidate.depthBias = std::clamp(light.shadowDepthBias, 0.f, 0.05f);
            shadowCandidate.normalBias = std::clamp(light.shadowNormalBias, 0.f, 0.1f);
            shadowCandidate.resolutionScale = std::clamp(
                light.shadowResolutionScale, 0.25f, 1.f);
            shadowCandidate.filterScale = std::clamp(light.shadowFilterScale, 0.f, 1.f);
            if (!selectedShadow || shadowCandidate.score > selectedShadow->score ||
                (shadowCandidate.score == selectedShadow->score &&
                    shadowCandidate.stableKey < selectedShadow->stableKey))
                selectedShadow = std::move(shadowCandidate);
        }

        if (selectedShadow && selectedShadow->lightIndex < count)
        {
            destination[selectedShadow->lightIndex].params.z = 0.f;
            destination[selectedShadow->lightIndex].params.w =
                selectedShadow->strength;
            shadowSelection->directionToLight =
                selectedShadow->directionToLight;
            shadowSelection->depthBias = selectedShadow->depthBias;
            shadowSelection->normalBias = selectedShadow->normalBias;
            shadowSelection->strength = selectedShadow->strength;
            shadowSelection->resolutionScale = selectedShadow->resolutionScale;
            shadowSelection->filterScale = selectedShadow->filterScale;
            shadowSelection->lightIndex = selectedShadow->lightIndex;
            shadowSelection->valid = true;
        }
        return count;
    }
}
