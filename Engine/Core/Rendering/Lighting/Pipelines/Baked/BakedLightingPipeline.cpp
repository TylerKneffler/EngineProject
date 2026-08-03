#include "BakedLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Light.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Engine::Rendering::Lighting
{
    namespace
    {
        struct BakeLight
        {
            glm::vec3 position{};
            glm::vec3 color{};
            float intensity = 0.f;
            float range = 0.f;
            float falloff = 2.f;
            Light::Type type = Light::Type::Point;
            glm::vec3 direction{ 0.f, 1.f, 0.f };
        };
    }

    BakeResult BakedLightingPipeline::Bake(Scene& scene) const
    {
        std::vector<BakeLight> lights;
        for (const auto& object : scene.GetObjects())
        {
            if (!object->IsEnabledInHierarchy())
                continue;
            const Light* light = object->GetComponent<Light>();
            if (!light || !light->baked || light->intensity <= 0.f)
                continue;
            if (light->GetLightType() == Light::Type::Point && light->range <= 0.f)
                continue;
            const glm::vec3 direction = light->GetLightType() == Light::Type::Ambient
                ? -glm::normalize(glm::vec3(object->transform.GetWorldMatrix()[2]))
                : glm::vec3(0.f, 1.f, 0.f);
            lights.push_back({
                object->transform.GetWorldPosition(), light->color,
                light->intensity, light->range, light->falloff,
                light->GetLightType(), direction });
        }

        uint32_t receivers = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (!object->GetComponent<Mesh>())
                continue;

            glm::vec3 irradiance(0.f);
            glm::vec3 directionalIrradiance(0.f);
            glm::vec3 weightedDirection(0.f);
            const glm::vec3 receiverPosition = object->transform.GetWorldPosition();
            for (const BakeLight& light : lights)
            {
                if (light.type == Light::Type::Ambient)
                {
                    const glm::vec3 contribution = light.color * light.intensity;
                    directionalIrradiance += contribution;
                    weightedDirection += light.direction *
                        (contribution.r + contribution.g + contribution.b);
                    continue;
                }
                const float distance = glm::length(light.position - receiverPosition);
                const float rangeFactor = std::max(
                    0.f, 1.f - distance / std::max(light.range, 0.0001f));
                const float attenuation = std::pow(
                    rangeFactor, std::max(light.falloff, 0.1f));
                irradiance += light.color * light.intensity * attenuation;
            }

            BakedLightingData* data = object->GetComponent<BakedLightingData>();
            if (!data)
                data = object->AddComponent<BakedLightingData>();
            data->irradiance = irradiance;
            data->directionalIrradiance = directionalIrradiance;
            data->lightDirection = glm::length(weightedDirection) > 0.0001f
                ? glm::normalize(weightedDirection)
                : glm::vec3(0.f, 1.f, 0.f);
            data->valid = true;
            data->version = 2;
            ++receivers;
        }

        BakeResult result{};
        result.succeeded = true;
        result.bakedLightCount = static_cast<uint32_t>(lights.size());
        result.receiverCount = receivers;
        result.message = "Baked " + std::to_string(result.bakedLightCount) +
            " light(s) into " + std::to_string(receivers) +
            " object irradiance record(s).";
        return result;
    }

    uint32_t BakedLightingPipeline::Clear(Scene& scene) const
    {
        uint32_t cleared = 0;
        for (const auto& object : scene.GetObjects())
        {
            for (auto component = object->Components.begin();
                component != object->Components.end();)
            {
                if (dynamic_cast<BakedLightingData*>(*component))
                {
                    delete *component;
                    component = object->Components.erase(component);
                    ++cleared;
                }
                else
                    ++component;
            }
        }
        return cleared;
    }
}
