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
    Triangles = 5,
    // Ground cover check: terrain renders red, everything drawn on it
    // (grass, shrubs, trees, props) green. Where foliage should hide the
    // ground, red showing through is bare terrain.
    Coverage = 6
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
    Reinhard = 2,
    // AgX (Troy Sobotka): gentler highlight roll-off and less saturated
    // brights than ACES; PostProcess.hlsl operator 6.
    AgX = 3
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
    // Shadowed in-scatter (light shafts): surfaces march the directional
    // cascade shadow map along their view ray, and shadowed stretches of fog
    // lose the sun glow. Costs lightShaftSamples shadow taps per fogged
    // pixel within the shadow range (8 samples: ~2 ms on the landscape
    // vista at 1080p); the sky is not shadowed. Opt-in for that reason.
    bool lightShafts = false;
    int lightShaftSamples = 8;
    // Share of the sun glow that shadow removes (0 = shafts off).
    float lightShaftStrength = 1.f;

    static constexpr int MaximumLightShaftSamples = 32;

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
        lightShaftSamples = std::clamp(lightShaftSamples, 0,
            MaximumLightShaftSamples);
        lightShaftStrength = std::clamp(lightShaftStrength, 0.f, 1.f);
    }
};

// Physically based sky and atmosphere (Rendering/Atmosphere/PhysicalAtmosphere,
// Hillaire 2020). When enabled it replaces the HDRI panorama as the sky and
// as the environment-lighting source, attenuates the main directional light
// by the atmosphere's transmittance, and (aerialPerspective) replaces the
// height fog with the atmosphere's aerial perspective.
// Eye adaptation (Graphics::PostProcessSettings auto exposure). EVs are log2
// of the metered average scene luminance, which maps to mid grey;
// outputExposure stays as compensation on top.
struct AutoExposureSettings
{
    bool enabled = false;
    float minEv = -8.f;
    float maxEv = 4.f;
    float speedUp = 3.f;    // 1/s, toward a brighter scene
    float speedDown = 1.f;  // 1/s, toward a darker scene
    float lowPercent = 0.5f;  // histogram share ignored at the dark end
    float highPercent = 0.95f; // ...and above this at the bright end
    // 0-1: how much metering favours the lower part of the frame (where the
    // ground is) over what fills the top (sky, a dark forest wall). At 1 the
    // top third counts a fifth as much, so a sunlit meadow keeps its
    // brightness whether a dark forest or open sky stands behind it.
    float groundWeight = 0.f;

    void Normalize()
    {
        groundWeight = std::clamp(groundWeight, 0.f, 1.f);
        minEv = std::clamp(minEv, -14.f, 10.f);
        maxEv = std::clamp(maxEv, minEv, 10.f);
        speedUp = std::clamp(speedUp, 0.f, 50.f);
        speedDown = std::clamp(speedDown, 0.f, 50.f);
        lowPercent = std::clamp(lowPercent, 0.f, 0.98f);
        highPercent = std::clamp(highPercent, lowPercent + 0.01f, 1.f);
    }
};

// Screen-space ambient occlusion (Shaders/AmbientOcclusion): GTAO at half
// resolution from the main view's depth, accumulated over frames, darkening
// the ambient and environment lighting of the next frame's surfaces.
struct AmbientOcclusionSettings
{
    bool enabled = false;
    float radius = 1.5f;       // metres
    float intensity = 1.f;     // exponent on the visibility (1 = as computed)
    float temporalBlend = 0.15f; // weight of each new frame against history

    void Normalize()
    {
        radius = std::clamp(radius, 0.1f, 20.f);
        intensity = std::clamp(intensity, 0.f, 4.f);
        temporalBlend = std::clamp(temporalBlend, 0.02f, 1.f);
    }
};

struct PhysicalSkySettings
{
    bool enabled = false;
    // World height of the planet surface ("sea level") the atmosphere
    // starts at; set near the lowest terrain.
    float groundHeight = 0.f;
    float rayleighScale = 1.f;   // air molecules: sky blue, distance blue
    float mieScale = 1.f;        // aerosols: haze, sun aureole, distance white
    float mieScaleHeight = 1200.f;
    float mieAnisotropy = 0.8f;
    float ozoneScale = 1.f;
    glm::vec3 groundAlbedo = glm::vec3(0.3f);
    // Multiplies the sky's radiance on screen (not its lighting).
    float skyIntensity = 1.f;
    bool attenuateSun = true;
    bool aerialPerspective = true;

    void Normalize()
    {
        rayleighScale = std::clamp(rayleighScale, 0.f, 20.f);
        mieScale = std::clamp(mieScale, 0.f, 200.f);
        mieScaleHeight = std::clamp(mieScaleHeight, 10.f, 20000.f);
        mieAnisotropy = std::clamp(mieAnisotropy, 0.f, 0.99f);
        ozoneScale = std::clamp(ozoneScale, 0.f, 20.f);
        groundAlbedo = glm::clamp(groundAlbedo, glm::vec3(0.f), glm::vec3(1.f));
        skyIntensity = std::clamp(skyIntensity, 0.f, 64.f);
    }
};

// Volumetric cloud layer ray-marched over the sky (Shaders/Clouds/Clouds.hlsl).
// Heights are world metres; the layer is flat (no planet curvature) and fades
// out toward maxDistance so the horizon blends into the sky behind it.
struct VolumetricCloudSettings
{
    bool enabled = false;
    // Share of the sky covered (0 clear .. 1 overcast).
    float coverage = 0.45f;
    // Extinction per metre inside a fully dense cloud.
    float density = 0.03f;
    float baseHeight = 2600.f;
    float thickness = 1400.f;
    // Size of the cloud shapes (metres per base noise period).
    float shapeScale = 2400.f;
    // How strongly fine noise erodes the cloud edges (0..1).
    float detail = 0.45f;
    // Drift in metres per second toward windDirection (degrees, 0 = +X).
    float windSpeed = 6.f;
    float windDirection = 30.f;
    // Sky light on the clouds (linear HDR), brighter toward their tops.
    glm::vec3 ambientColor = glm::vec3(0.55f, 0.62f, 0.75f);
    float ambientIntensity = 0.8f;
    // Scales the sun's contribution.
    float sunIntensity = 1.f;
    // March steps per pixel (quality vs GPU cost) and the farthest cloud drawn.
    int steps = 48;
    float maxDistance = 30000.f;

    void Normalize()
    {
        coverage = std::clamp(coverage, 0.f, 1.f);
        density = std::clamp(density, 0.f, 1.f);
        thickness = std::clamp(thickness, 1.f, 20000.f);
        shapeScale = std::clamp(shapeScale, 10.f, 100000.f);
        detail = std::clamp(detail, 0.f, 1.f);
        windSpeed = std::clamp(windSpeed, 0.f, 200.f);
        ambientColor = glm::max(ambientColor, glm::vec3(0.f));
        ambientIntensity = std::max(0.f, ambientIntensity);
        sunIntensity = std::max(0.f, sunIntensity);
        steps = std::clamp(steps, 8, 128);
        maxDistance = std::clamp(maxDistance, 100.f, 200000.f);
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
    AutoExposureSettings autoExposure;
    AmbientOcclusionSettings ambientOcclusion;
    AtmosphereFogSettings atmosphereFog;
    VolumetricCloudSettings volumetricClouds;
    PhysicalSkySettings physicalSky;
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

