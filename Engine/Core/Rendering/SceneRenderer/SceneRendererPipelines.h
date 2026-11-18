#pragma once

#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/RenderUtilities.h"

namespace Engine::Rendering
{
class SceneRendererPipelines
{
public:
    static void BuildSkyboxPipeline(SceneRenderer& renderer);
    static void BuildGridPipeline(SceneRenderer& renderer);
    static void BuildParticlePipeline(SceneRenderer& renderer);
    static void UpdateEnvironmentLighting(SceneRenderer& renderer, const Engine::Components::Texture* texture);
    static std::shared_ptr<Engine::Rendering::EnvironmentMap> ResolveReflectionEnvironment(
        SceneRenderer& renderer, const Engine::Components::Material& material);
};
}
