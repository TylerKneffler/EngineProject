#include "Core/Scene/Scene.h"

Engine::Rendering::Lighting::BakeResult Scene::BakeLighting()
{
    auto result = m_bakedLightingPipeline.Bake(*this);
    m_lightingBakeStatus = result.message;
    return result;
}

void Scene::ClearBakedLighting()
{
    const uint32_t count = m_bakedLightingPipeline.Clear(*this);
    m_lightingBakeStatus = "Cleared " + std::to_string(count) +
        " baked lighting record(s).";
}
