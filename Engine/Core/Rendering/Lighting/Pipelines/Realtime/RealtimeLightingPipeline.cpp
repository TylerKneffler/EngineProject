#include "RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/common.hpp>
#include <optional>
#include <vector>

namespace Engine::Rendering
{
    constexpr float Pi = 3.14159265358979323846f;
    struct RealtimeLightingPipeline::Scratch
    {
        struct LightCandidate
        {
            LightData data{};
            const Engine::Components::Light* light = nullptr;
            std::string stableKey;
            float importance = 0.f;
        };

        std::vector<LightCandidate> candidates;
    };

    RealtimeLightingPipeline::RealtimeLightingPipeline()
        : m_scratch(std::make_unique<Scratch>())
    {
    }

    RealtimeLightingPipeline::~RealtimeLightingPipeline() = default;

    uint32_t RealtimeLightingPipeline::CollectLights(
        const Engine::Scene::Scene& scene,
        LightData* destination,
        uint32_t capacity,
        bool mapThroughSpatialVolumes,
        const Engine::Model::RealtimeShadowSettings* shadowSettings,
        Engine::Model::RealtimeShadowSelection* shadowSelection,
        const glm::vec3* importancePosition,
        std::array<std::string, 4>* cookieTextures,
        std::array<std::string, 4>* iesProfiles) const
    {
        if (cookieTextures) cookieTextures->fill({});
        if (iesProfiles) iesProfiles->fill({});
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

        std::vector<Scratch::LightCandidate>& candidates =
            m_scratch->candidates;
        candidates.clear();
        candidates.reserve(scene.GetObjects().size() * 2u);
        const auto importanceScore = [&](const LightData& data,
            const Engine::Components::Light& light)
        {
            const float radiance = std::max(0.f, data.colorIntensity.w) *
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
            if (light->GetLightType() != Engine::Components::Light::Type::Directional && light->range <= 0.f)
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
            const auto type = light->GetLightType();
            const glm::vec3 rayDirection = glm::normalize(glm::vec3(world[2]));
            if (light->GetLightType() == Engine::Components::Light::Type::Directional)
            {
                data.positionRange = glm::vec4(-rayDirection, 0.f);
            }
            else
            {
                data.positionRange = glm::vec4(
                    glm::vec3(world[3]), light->range);
            }
            const float outerRadians = glm::radians(std::clamp(
                light->outerConeAngle, 0.1f, 89.f));
            const float innerRadians = glm::radians(std::clamp(
                light->innerConeAngle, 0.f, light->outerConeAngle));
            float gpuIntensity = light->intensity;
            const bool physical = light->GetIntensityMode() ==
                Engine::Components::Light::IntensityMode::Physical;
            if (physical && type == Engine::Components::Light::Type::Point)
                gpuIntensity /= 4.f * Pi;
            else if (physical && type == Engine::Components::Light::Type::Spot)
                gpuIntensity /= std::max(2.f * Pi *
                    (1.f - std::cos(outerRadians)), 0.0001f);
            data.colorIntensity = glm::vec4(light->color, gpuIntensity);
            data.params = glm::vec4(light->falloff, static_cast<float>(type),
                -1.f, 0.f);
            data.directionCone = glm::vec4(rayDirection, std::cos(outerRadians));
            data.attenuation = glm::vec4(std::cos(innerRadians),
                physical ? 1.f : 0.f,
                glm::uintBitsToFloat(static_cast<uint32_t>(light->lightingChannels)), 0.f);
            data.cookieRight = glm::vec4(glm::normalize(glm::vec3(world[0])), 0.f);
            data.cookieUp = glm::vec4(glm::normalize(glm::vec3(world[1])), 0.f);
            data.photometry.z = std::max(light->cookieScale, 0.001f);
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
                if (type == Engine::Components::Light::Type::Spot)
                    mapped.directionCone = glm::vec4(glm::normalize(glm::vec3(
                        connection.localToRemote * glm::vec4(
                            glm::vec3(data.directionCone), 0.f))),
                        data.directionCone.w);
                mapped.cookieRight = glm::vec4(glm::normalize(glm::vec3(
                    connection.localToRemote * glm::vec4(
                        glm::vec3(data.cookieRight), 0.f))), 0.f);
                mapped.cookieUp = glm::vec4(glm::normalize(glm::vec3(
                    connection.localToRemote * glm::vec4(
                        glm::vec3(data.cookieUp), 0.f))), 0.f);
            }

            candidates.push_back({ mapped, light, stableKey + "/mapped",
                importanceScore(mapped, *light) });
        }

        std::stable_sort(candidates.begin(), candidates.end(),
            [](const Scratch::LightCandidate& left,
                const Scratch::LightCandidate& right)
            {
                constexpr float epsilon = 0.000001f;
                if (std::abs(left.importance - right.importance) > epsilon)
                    return left.importance > right.importance;
                return left.stableKey < right.stableKey;
            });
        const uint32_t count = std::min(capacity,
            static_cast<uint32_t>(candidates.size()));
        const auto assignTexture = [](const std::string& path,
            std::array<std::string, 4>* table) -> float
        {
            if (path.empty() || !table) return 0.f;
            for (size_t index = 0; index < table->size(); ++index)
            {
                if ((*table)[index] == path) return static_cast<float>(index + 1u);
                if ((*table)[index].empty())
                { (*table)[index] = path; return static_cast<float>(index + 1u); }
            }
            return 0.f;
        };
        for (uint32_t index = 0; index < count; ++index)
        {
            destination[index] = candidates[index].data;
            const Engine::Components::Light& light = *candidates[index].light;
            destination[index].photometry.x = assignTexture(
                light.cookieTexture, cookieTextures);
            destination[index].photometry.y = assignTexture(
                light.iesProfileTexture, iesProfiles);
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
            shadowSelection->stableLightKey = selectedShadow->stableKey;
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
