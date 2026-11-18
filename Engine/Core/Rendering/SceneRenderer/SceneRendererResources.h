#pragma once

#include <cstdint>

#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{
class SceneRendererResources
{
public:
    static void Initialize(SceneRenderer& renderer, Engine::Graphics::IGraphicsProvider* graphicsProvider);
    static void EnsureObjectRenderCapacity(SceneRenderer& renderer, uint32_t requiredObjects);
    static void SelectRenderPassBuffers(SceneRenderer& renderer, uint32_t requiredObjectSlots);
    static void EnsureSkinPaletteCapacity(SceneRenderer& renderer, uint32_t requiredMatrices);
};
}
