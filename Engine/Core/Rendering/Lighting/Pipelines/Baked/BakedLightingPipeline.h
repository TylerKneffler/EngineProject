#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Model/LightingData.h"
#include <string>
#include <functional>

namespace Engine::Scene { class Scene; }

namespace Engine::Rendering
{
    class BakedLightingPipeline final : public ILightingPipeline
    {
    public:
        using BakeResult = Engine::Model::BakeResult;
        using BakedLightingSettings = Engine::Model::BakedLightingSettings;

        LightingPipelineKind GetKind() const override
        {
            return LightingPipelineKind::Baked;
        }

        BakeResult Bake(Engine::Scene::Scene& scene, const std::string& assetsDirectory,
            const std::string& sceneName,
            const BakedLightingSettings& bakeSettings,
            const std::function<void(float)>& progress = {},
            const std::function<bool()>& cancelled = {}) const;
        uint32_t Clear(Engine::Scene::Scene& scene) const;
    };
}
