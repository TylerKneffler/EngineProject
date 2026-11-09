#pragma once

#include <cstddef>

#include "Core/Scene/Scene.h"

namespace Engine::Scene::Renderer
{
class SceneRendererDebugPipelines
{
public:
    static void BuildObjectDebugPipelines(
        Scene& scene,
        const Engine::Graphics::IShader* vsShader,
        const Engine::Graphics::IShader* psShader,
        const Engine::Graphics::IPipelineStateBuilder::VertexElement* layout,
        size_t layoutCount,
        const Engine::Graphics::IPipelineStateBuilder::VertexElement* terrainLayout,
        size_t terrainLayoutCount);
};
}
