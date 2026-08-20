#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Engine::Model
{
enum class SceneRenderMode
{
    Lit = 0,
    Unlit = 1,
    Wireframe = 2
};

struct SceneSettings
{
    bool showGrid = true;
    int gridHalfSize = 10;
    float gridCellSize = 1.f;
    float gridOpacity = 0.4f;
    float gridFadeDistance = 80.f;
    glm::vec3 gridColor = glm::vec3(0.45f, 0.45f, 0.45f);
    glm::vec3 gridOriginColor = glm::vec3(0.30f, 0.50f, 0.80f);
    glm::vec3 ambientColor = glm::vec3(0.12f, 0.12f, 0.12f);
    std::string skyboxTexture;
    // The equirectangular sky texture can also drive image-based diffuse
    // lighting and reflections. Exposure is measured in EV and rotation in
    // degrees around the world up axis.
    bool hdriLightingEnabled = false;
    float hdriIntensity = 1.f;
    float hdriExposure = 0.f;
    float hdriRotation = 0.f;
    SceneRenderMode renderMode = SceneRenderMode::Lit;
    // When true, SceneView also shows game-style screen-space UI composition.
    // Default stays false so scene camera reflects in-scene editing context.
    bool sceneViewUiOverlay = false;

};
}

