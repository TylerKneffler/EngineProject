#include "Core/Rendering/SceneRenderer.h"

namespace Engine::Rendering
{

Engine::Model::BakeResult SceneRenderer::BakeLighting(
    const std::string& assetsDirectory, const std::string& sceneName,
    const Engine::Model::BakedLightingSettings& bakeSettings,
    const std::function<void(float)>& progress,
    const std::function<bool()>& cancelled)
{
    auto result = m_bakedLightingPipeline.Bake(
        m_scene, assetsDirectory, sceneName, bakeSettings, progress, cancelled);
    m_lightingBakeStatus = result.message;
    return result;
}

void SceneRenderer::ClearBakedLighting()
{
    const uint32_t count = m_bakedLightingPipeline.Clear(m_scene);
    m_lightingBakeStatus = "Cleared " + std::to_string(count) +
        " object(s) to their original material(s). Generated assets were retained.";
}

}
