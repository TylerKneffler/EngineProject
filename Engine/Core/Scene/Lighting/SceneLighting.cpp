#include "Core/Scene/Scene.h"

Engine::Rendering::Lighting::BakeResult Scene::BakeLighting(
    const std::string& assetsDirectory, const std::string& sceneName,
    const Engine::Rendering::Lighting::BakedLightingSettings& bakeSettings)
{
    auto result = m_bakedLightingPipeline.Bake(
        *this, assetsDirectory, sceneName, bakeSettings);
    m_lightingBakeStatus = result.message;
    return result;
}

void Scene::ClearBakedLighting()
{
    const uint32_t count = m_bakedLightingPipeline.Clear(*this);
    m_lightingBakeStatus = "Cleared " + std::to_string(count) +
        " object(s) to their original material(s). Generated assets were retained.";
}
