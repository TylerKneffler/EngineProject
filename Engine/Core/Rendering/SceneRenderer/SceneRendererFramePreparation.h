#pragma once

#include "Core/Rendering/SceneRenderer.h"
#include "Core/Components/Materials/Material.h"

#include <optional>

namespace Engine::Rendering
{
class SceneRendererFramePreparation
{
public:
    using Scene = Engine::Scene::Scene;
    // `validated`: the frame's parallel pre-pass already ran Material::Validate
    // on this object's own material (prefab LOD materials are not covered).
    // `texturesNeeded` / `shadersNeeded` (only meaningful when validated): the
    // pre-pass's NeedsTexturePreparation / NeedsShaderPrewarm results.
    static void PrepareItemMaterial(SceneRenderer& renderer, SceneRenderer::FrameRenderItem& item,
        bool validated = false, bool texturesNeeded = true, bool shadersNeeded = true);
    static void PrepareFrameItems(
        SceneRenderer& renderer,
        uint64_t warpRevision,
        bool hasActiveWarpVolume, bool preparedTransforms,
        std::optional<Scene::SpatialWarpSnapshot>& warpSnapshot);

    static void PrepareSpatialBounds(SceneRenderer& renderer);
private:
    static Engine::Components::Material::ShaderPrewarmEpoch ShaderPrewarmEpoch(const SceneRenderer& renderer);
};
}
