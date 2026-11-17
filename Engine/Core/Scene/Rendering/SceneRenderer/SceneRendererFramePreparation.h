#pragma once

#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Materials/Material.h"

#include <optional>

namespace Engine::Scene::Renderer
{
class SceneRendererFramePreparation
{
public:
    // `validated`: the frame's parallel pre-pass already ran Material::Validate
    // on this object's own material (prefab LOD materials are not covered).
    // `texturesNeeded` / `shadersNeeded` (only meaningful when validated): the
    // pre-pass's NeedsTexturePreparation / NeedsShaderPrewarm results.
    static void PrepareItemMaterial(Scene& scene, Scene::FrameRenderItem& item,
        bool validated = false, bool texturesNeeded = true, bool shadersNeeded = true);
    static void PrepareFrameItems(
        Scene& scene,
        uint64_t warpRevision,
        bool hasActiveWarpVolume, bool preparedTransforms,
        std::optional<Scene::SpatialWarpSnapshot>& warpSnapshot);

    static void PrepareSpatialBounds(Scene& scene);
private:
    static Engine::Components::Material::ShaderPrewarmEpoch ShaderPrewarmEpoch(const Scene& scene);
};
}
