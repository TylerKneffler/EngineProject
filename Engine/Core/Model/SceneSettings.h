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

enum class SceneDimension
{
    ThreeD = 0,
    TwoD = 1
};

struct SceneSettings
{
    // Dimensional editing and rendering behavior belongs to the scene so one
    // project can freely contain both 2D and 3D scenes.
    SceneDimension dimension = SceneDimension::ThreeD;
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

    // Editor-only debug overlays for portals, matrix links, and warp volumes.
    bool portalDebugVisuals = false;
    // Lets the editor Scene camera use the game camera's source-chart
    // look-through behavior when its ray enters a finite warp volume. Keep
    // this off by default so the Scene camera shows the authored bent space.
    bool sceneCameraWarpLookThrough = false;
    bool portalDebugWireframe = false;
    bool portalDebugTintRemoteView = true;
    float portalDebugOverlayAlpha = 0.45f;
    // One level renders the directly connected side. Higher values render
    // portals visible through portals. Runtime clamps both values to hard
    // safety limits before scheduling any work.
    int portalRecursionDepth = 2;
    // Maximum visits to one linked portal pair along a recursive view path.
    // Two shows the source scene once through its target; higher values extend
    // that A -> B -> A loop while recursion depth remains the global limit.
    int portalConnectionRepeatLimit = 2;
    int portalMaxViewsPerFrame = 24;

};
}

