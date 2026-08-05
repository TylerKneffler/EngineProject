#include "RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Light.h"

namespace Engine::Rendering::Lighting
{
    uint32_t RealtimeLightingPipeline::CollectLights(
        const Scene& scene,
        LightData* destination,
        uint32_t capacity) const
    {
        if (!destination || capacity == 0)
            return 0;

        uint32_t count = 0;
        for (const auto& candidate : scene.GetObjects())
        {
            if (!candidate->IsEnabledInHierarchy() || count >= capacity)
                continue;
            const Light* light = candidate->GetComponent<Light>();
            if (!light || light->baked || light->intensity <= 0.f)
                continue;
            if (light->GetLightType() == Light::Type::Point && light->range <= 0.f)
                continue;

            LightData& data = destination[count++];
            if (light->GetLightType() == Light::Type::Ambient)
            {
                const glm::mat4 world = candidate->transform.GetWorldMatrix();
                const glm::vec3 rayDirection = glm::normalize(glm::vec3(world[2]));
                data.positionRange = glm::vec4(-rayDirection, 0.f);
            }
            else
            {
                data.positionRange = glm::vec4(
                    candidate->transform.GetWorldPosition(), light->range);
            }
            data.colorIntensity = glm::vec4(light->color, light->intensity);
            data.params = glm::vec4(light->falloff,
                light->GetLightType() == Light::Type::Ambient ? 1.f : 0.f,
                0.f, 0.f);
        }
        return count;
    }
}
