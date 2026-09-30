#pragma once

#include "Core/Rendering/Lighting/Pipelines/ILightingPipeline.h"
#include "Core/Model/LightingData.h"
#include <cstdint>
#include <memory>
#include <array>
#include <string>

namespace Engine::Scene { class Scene; }

namespace Engine::Rendering
{
    class RealtimeLightingPipeline final : public ILightingPipeline
    {
    public:
        using LightData = Engine::Model::LightData;

        RealtimeLightingPipeline();
        ~RealtimeLightingPipeline();

        LightingPipelineKind GetKind() const override
        {
            return LightingPipelineKind::Realtime;
        }

        uint32_t CollectLights(
            const Engine::Scene::Scene& scene,
            LightData* destination,
            uint32_t capacity,
            bool mapThroughSpatialVolumes = true,
            const Engine::Model::RealtimeShadowSettings* shadowSettings = nullptr,
            Engine::Model::RealtimeShadowSelection* shadowSelection = nullptr,
            const glm::vec3* importancePosition = nullptr,
            std::array<std::string, 4>* cookieTextures = nullptr,
            std::array<std::string, 4>* iesProfiles = nullptr) const;

    private:
        struct Scratch;
        mutable std::unique_ptr<Scratch> m_scratch;
    };
}
