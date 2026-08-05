#pragma once

namespace Engine::Rendering::Lighting
{
    enum class LightingPipelineKind
    {
        Realtime,
        Baked
    };

    class ILightingPipeline
    {
    public:
        virtual ~ILightingPipeline() = default;
        virtual LightingPipelineKind GetKind() const = 0;
    };
}
