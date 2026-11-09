#pragma once

#include <cstdint>

#include "Core/Scene/Scene.h"

namespace Engine::Scene::Renderer
{
class SceneRendererResources
{
public:
    static void Initialize(Scene& scene, Engine::Graphics::IGraphicsProvider* graphicsProvider);
    static void EnsureObjectRenderCapacity(Scene& scene, uint32_t requiredObjects);
    static void SelectRenderPassBuffers(Scene& scene, uint32_t requiredObjectSlots);
    static void EnsureSkinPaletteCapacity(Scene& scene, uint32_t requiredMatrices);
};
}
