#include "RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <glm/geometric.hpp>

namespace Engine::Rendering
{
    uint32_t RealtimeLightingPipeline::CollectLights(
        const Engine::Scene::Scene& scene,
        LightData* destination,
        uint32_t capacity,
        bool mapThroughSpatialVolumes) const
    {
        if (!destination || capacity == 0)
            return 0;

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
            if (light->GetLightType() == Engine::Components::Light::Type::Ambient)
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
            data.params = glm::vec4(light->falloff,
                light->GetLightType() == Engine::Components::Light::Type::Ambient ? 1.f : 0.f,
                0.f, 0.f);

            destination[count++] = data;

            if (count >= capacity)
                continue;

            const Engine::Components::MatrixLayerConnection& connection =
                candidate->transform.matrixLayer.connection;
            if (!connection.enabled)
                continue;

            LightData mapped = data;
            if (light->GetLightType() == Engine::Components::Light::Type::Ambient)
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
        return count;
    }
}
