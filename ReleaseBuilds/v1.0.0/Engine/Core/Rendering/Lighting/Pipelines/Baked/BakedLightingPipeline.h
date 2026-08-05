#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Rendering/Lighting/LightingTypes.h"

class Scene;

namespace Engine::Rendering::Lighting
{
    class BakedLightingPipeline final : public ILightingPipeline
    {
    public:
        LightingPipelineKind GetKind() const override
        {
            return LightingPipelineKind::Baked;
        }

        BakeResult Bake(Scene& scene) const;
        uint32_t Clear(Scene& scene) const;
    };
}
