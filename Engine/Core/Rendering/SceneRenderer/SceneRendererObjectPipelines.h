#pragma once

#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{
class SceneRendererObjectPipelines
{
public:
    static void BuildObjectPipeline(SceneRenderer& renderer);
};
}
