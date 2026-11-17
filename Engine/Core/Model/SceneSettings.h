#pragma once

#include "Core/Model/RendererData.h"
#include "Core/Model/TextureLodSettings.h"
#include "Core/Model/GraphicsQualitySettings.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <string>

namespace Engine::Model
{
enum class SceneRenderMode
{
    Lit = 0,
    Unlit = 1,
    Wireframe = 2,
    // Editor-only: blue-to-red heat map of per-draw CPU + GPU cost.
    Performance = 3,
    // Blue-to-red per-pixel detail: instanced LOD band and resident texture
    // resolution against the screen. Close range should read red.
    Detail = 4,
    // Editor-only: every triangle in its own distinct colour. Many small
    // patches mean dense geometry; large flat patches mean little detail.
    Triangles = 5
};

enum class SceneDimension
{
    ThreeD = 0,
    TwoD = 1
};

enum class ToneMappingOperator
{
    None = 0,
    Aces = 1,
    Reinhard = 2
};

// Analytic exponential height fog applied per pixel by every forward shader
// and the skybox (Shaders/AtmosphereFog.hlsli). Distances are scene units
// (metres in the landscape scenes).
struct AtmosphereFogSettings
{
    bool enabled = false;
    // Extinction per unit distance at baseHeight.
    float density = 0.0002f;
    // Density falls off as exp(-heightFalloff * (y - baseHeight)).
    float heightFalloff = 0.004f;
    float baseHeight = 0.f;
    // Fog starts this far from the camera (keeps the foreground crisp).
    float startDistance = 0.f;
    // Upper bound on fog opacity for surfaces; 1 lets distant ridges fully
    // dissolve into the haze.
    float maxOpacity = 1.f;
    // Linear HDR color of ambient (sky-lit) in-scatter.
    glm::vec3 inscatterColor = glm::vec3(0.55f, 0.65f, 0.8f);
    float inscatterIntensity = 1.f;
    // Henyey-Greenstein forward-scatter glow around the main directional
    // light, tinted by that light's color.
    float sunGlowStrength = 0.25f;
    float sunGlowAnisotropy = 0.7f;
    // Length of the view ray used to haze the sky toward the horizon.
    float skyDistance = 30000.f;

    void Normalize()
    {
        density = std::clamp(density, 0.f, 1.f);
        heightFalloff = std::clamp(heightFalloff, 0.f, 1.f);
        startDistance = std::max(0.f, startDistance);
        maxOpacity = std::clamp(maxOpacity, 0.f, 1.f);
        inscatterColor = glm::max(inscatterColor, glm::vec3(0.f));
        inscatterIntensity = std::max(0.f, inscatterIntensity);
        sunGlowStrength = std::max(0.f, sunGlowStrength);
        sunGlowAnisotropy = std::clamp(sunGlowAnisotropy, 0.f, 0.95f);
        skyDistance = std::clamp(skyDistance, 0.f, 1000000.f);
    }
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
    bool skyboxEnabled = true;
    std::string skyboxTexture;
    // The equirectangular sky texture can also drive image-based diffuse
    // lighting and reflections. Exposure is measured in EV and rotation in
    // degrees around the world up axis.
    bool hdriLightingEnabled = false;
    float hdriIntensity = 1.f;
    float hdriExposure = 0.f;
    float hdriRotation = 0.f;
    ToneMappingOperator toneMapping = ToneMappingOperator::Aces;
    float outputExposure = 0.f;
    AtmosphereFogSettings atmosphereFog;
    // Energy-conserving bloom: the compose pass mixes this fraction of a
    // wide, multi-scale blur of the HDR scene into the image before tone
    // mapping. Bright pixels (the sun disc, speculars) bleed the most.
    bool bloomEnabled = true;
    float bloomIntensity = 0.04f;
    // Split-tone grade applied after tone mapping: shadows are multiplied by
    // shadowTint and highlights by highlightTint, split around mid grey
    // (balance shifts the split, -1..1).
    bool colorGradingEnabled = false;
    glm::vec3 gradingShadowTint = glm::vec3(0.90f, 0.96f, 1.08f);
    glm::vec3 gradingHighlightTint = glm::vec3(1.06f, 0.99f, 0.90f);
    float gradingBalance = 0.f;
    float gradingContrast = 1.f;
    float gradingSaturation = 1.f;
    // Sun disc and forward-scattering glow drawn by the skybox toward the
    // brightest directional light. Intensity scales that light's radiance.
    bool sunDiscEnabled = false;
    float sunDiscAngularDiameter = 0.53f; // degrees
    float sunDiscIntensity = 40.f;
    float sunGlowIntensity = 0.35f;
    SceneRenderMode renderMode = SceneRenderMode::Lit;
    GraphicsQualitySettings graphicsQuality;
    TextureLodSettings textureLod;
    // Heavy scenes can optionally render at a reduced internal resolution and
    // optionally combine it with temporal reconstruction for stable output.
    RenderScaleMode renderScaleMode = RenderScaleMode::Native;
    float renderScale = 1.0f;
    bool dynamicInternalRenderScale = false;
    bool temporalUpscaling = false;
    float renderScaleTargetMs = 16.67f;
    float renderScaleMinimum = 0.5f;
    float renderScaleMaximum = 1.0f;
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

