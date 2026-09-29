#include "RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <optional>

namespace Engine::Rendering
{
    uint32_t RealtimeLightingPipeline::CollectLights(
        const Engine::Scene::Scene& scene,
        LightData* destination,
        uint32_t capacity,
        bool mapThroughSpatialVolumes,
        const Engine::Model::RealtimeShadowSettings* shadowSettings,
        Engine::Model::RealtimeShadowSelection* shadowSelection) const
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

        uint32_t count = 0;
        for (const auto& candidate : scene.GetObjects())
        {
            if (!candidate->IsEnabledInHierarchy() || count >= capacity)
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
            const bool shadowEligible = directional && light->castsShadows &&
                shadowSettings && shadowSettings->enabled &&
                shadowSettings->maximumShadowedLights > 0u && shadowSelection;
            data.params = glm::vec4(light->falloff, directional ? 1.f : 0.f,
                -1.f, 0.f);

            if (shadowEligible)
            {
                ShadowCandidate shadowCandidate{};
                shadowCandidate.lightIndex = count;
                shadowCandidate.strength = std::clamp(
                    light->shadowStrength, 0.f, 1.f);
                shadowCandidate.score = std::max(0.f, light->intensity) *
                    std::max({ 0.f, light->color.r, light->color.g,
                        light->color.b }) * shadowCandidate.strength;
                shadowCandidate.stableKey = stableObjectPath(candidate.get());
                shadowCandidate.directionToLight = glm::normalize(
                    glm::vec3(data.positionRange));
                shadowCandidate.depthBias = std::clamp(
                    light->shadowDepthBias, 0.f, 0.05f);
                shadowCandidate.normalBias = std::clamp(
                    light->shadowNormalBias, 0.f, 0.1f);
                shadowCandidate.resolutionScale = std::clamp(
                    light->shadowResolutionScale, 0.25f, 1.f);
                shadowCandidate.filterScale = std::clamp(
                    light->shadowFilterScale, 0.f, 1.f);
                constexpr float scoreEpsilon = 0.000001f;
                if (!selectedShadow ||
                    shadowCandidate.score > selectedShadow->score + scoreEpsilon ||
                    (std::abs(shadowCandidate.score - selectedShadow->score) <=
                        scoreEpsilon &&
                        shadowCandidate.stableKey < selectedShadow->stableKey))
                {
                    selectedShadow = std::move(shadowCandidate);
                }
            }

            destination[count++] = data;

            if (count >= capacity)
                continue;

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

            destination[count++] = mapped;
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
