#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Model/LightingData.h"
#include <string>

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

        BakeResult Bake(Scene& scene, const std::string& assetsDirectory,
            const std::string& sceneName,
            const BakedLightingSettings& bakeSettings) const;
        uint32_t Clear(Scene& scene) const;
    };
}
