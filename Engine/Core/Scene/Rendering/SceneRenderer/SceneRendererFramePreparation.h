#pragma once

#include "Core/Scene/Scene.h"

#include <optional>

namespace Engine::Scene::Renderer
{
class SceneRendererFramePreparation
{
public:
    static void PrepareItemMaterial(Scene& scene, Scene::FrameRenderItem& item);
    static void PrepareFrameItems(
        Scene& scene,
        uint64_t warpRevision,
        bool hasActiveWarpVolume, bool preparedTransforms,
        std::optional<Scene::SpatialWarpSnapshot>& warpSnapshot);

    static void PrepareSpatialBounds(Scene& scene);
};
}
