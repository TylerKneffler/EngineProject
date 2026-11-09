#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererVisibility.h"

namespace Engine::Scene::Renderer
{
class SceneRenderer
{
public:
    using VisibilityState = SceneRendererVisibility::ViewState;
    using LightingConfig = SceneRendererLighting::CascadeConfig;

    explicit SceneRenderer(VisibilityState visibilityState = {}, LightingConfig lightingConfig = {})
        : m_visibility(visibilityState), m_lighting(lightingConfig)
    {
    }

    void PrepareFrame()
    {
        m_visibility.Prepare(m_visibilityState);
        m_lighting.Prepare(m_lightingConfig);
    }

    void BuildVisibility()
    {
        // Future: resolve draw list and visible object ranges here.
    }

    void UpdateLighting()
    {
        // Future: gather cascades, shadow maps, and light policies here.
    }

    void SetVisibilityState(const VisibilityState& state)
    {
        m_visibilityState = state;
    }

    void SetLightingConfig(const LightingConfig& config)
    {
        m_lightingConfig = config;
    }

    const VisibilityState& GetVisibilityState() const
    {
        return m_visibilityState;
    }

    const LightingConfig& GetLightingConfig() const
    {
        return m_lightingConfig;
    }

private:
    VisibilityState m_visibilityState;
    LightingConfig m_lightingConfig;
    SceneRendererVisibility m_visibility;
    SceneRendererLighting m_lighting;
};
}
