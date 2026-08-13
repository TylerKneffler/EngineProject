#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Model/LightingData.h"
#include <cstdint>

namespace Engine::Scene { class Scene; }

namespace Engine::Rendering
{
    class RealtimeLightingPipeline final : public ILightingPipeline
    {
    public:
        using LightData = Engine::Model::LightData;

        LightingPipelineKind GetKind() const override
        {
            return LightingPipelineKind::Realtime;
        }

        uint32_t CollectLights(
            const Engine::Scene::Scene& scene,
            LightData* destination,
            uint32_t capacity) const;
    };
}
