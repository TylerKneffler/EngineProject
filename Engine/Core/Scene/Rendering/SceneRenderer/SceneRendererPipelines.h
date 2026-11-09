#pragma once

#include "Core/Scene/Scene.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

namespace Engine::Scene::Renderer
{
class SceneRendererPipelines
{
public:
    static void BuildSkyboxPipeline(Scene& scene);
    static void BuildGridPipeline(Scene& scene);
    static void BuildParticlePipeline(Scene& scene);
    static void UpdateEnvironmentLighting(Scene& scene, const Engine::Components::Texture* texture);
    static std::shared_ptr<Engine::Rendering::EnvironmentMap> ResolveReflectionEnvironment(
        Scene& scene, const Engine::Components::Material& material);
};
}
