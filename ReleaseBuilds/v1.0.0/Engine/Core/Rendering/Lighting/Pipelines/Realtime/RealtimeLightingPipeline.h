#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Rendering/Lighting/LightingTypes.h"
#include <cstdint>

class Scene;

namespace Engine::Rendering::Lighting
{
    class RealtimeLightingPipeline final : public ILightingPipeline
    {
    public:
        LightingPipelineKind GetKind() const override
        {
            return LightingPipelineKind::Realtime;
        }

        uint32_t CollectLights(
            const Scene& scene,
            LightData* destination,
            uint32_t capacity) const;
    };
}
