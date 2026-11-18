#pragma once

#include <cstddef>

#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{
class SceneRendererDebugPipelines
{
public:
    static void BuildObjectDebugPipelines(
        SceneRenderer& renderer,
        const Engine::Graphics::IShader* vsShader,
        const Engine::Graphics::IShader* psShader,
        const Engine::Graphics::IPipelineStateBuilder::VertexElement* layout,
        size_t layoutCount,
        const Engine::Graphics::IPipelineStateBuilder::VertexElement* terrainLayout,
        size_t terrainLayoutCount);
};
}
