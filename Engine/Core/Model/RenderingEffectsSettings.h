#pragma once

#include <algorithm>

namespace Engine::Model
{
// Project-wide image quality and look controls (Preferences > Rendering
// Effects, saved in the .proj file). Scenes author their own look (bloom,
// grading, exposure, ambient occlusion); these switches and scales apply on
// top of every scene, so a project can trade looks for speed in one place.

// Anti-aliasing. FXAA runs inside the final compose pass on every backend;
// the grass minimum width keeps distant blades from thinning below a pixel,
// which is what makes them sparkle and break up into dots.
struct AntiAliasingSettings
{
    bool fxaa = true;
    // How much FXAA also softens detail smaller than a pixel (0 keeps it
    // sharp, 1 smooths it fully). Above ~0.5 the image turns visibly soft.
    float fxaaSubpixel = 0.5f;
    // Minimum local contrast treated as an edge, as a share of the brightest
    // neighbour. Lower catches fainter edges at some cost in sharpness.
    float fxaaEdgeThreshold = 0.125f;
    // Hardware multisampling of the 3D scene: 1 (off), 2, 4 or 8 samples.
    // Smooths geometric edges (grass blades, foliage, silhouettes) at the
    // source; costs memory bandwidth, so it is off by default. Renderers
    // that do not support a count fall back to the nearest one they do.
    unsigned msaaSamples = 1;
    // Temporal anti-aliasing: the camera jitters by a sub-pixel offset every
    // frame and each pixel blends with its reprojected history, which also
    // removes the shimmer of thin moving detail (grass). Needs single-sample
    // depth, so it is not combined with MSAA (MSAA wins when both are set).
    // On by default: instanced LOD cross-fades and impostor view hand-overs
    // dither per pixel and rely on the history to blend (Object.hlsl
    // ScreenDitherThreshold); without it they show a fixed dot pattern.
    bool taa = true;
    // Share of the history kept each frame (0.75 responsive .. 0.97 smooth).
    float taaFeedback = 0.9f;
    // Restores detail TAA softens (most visible while moving): a light
    // unsharp mask in the final compose. 0 is off.
    float taaSharpen = 0.25f;
    bool grassMinimumWidth = true;
    // Grass blades never draw narrower than this many pixels.
    float grassMinimumPixels = 1.f;

    void Clamp()
    {
        fxaaSubpixel = std::clamp(fxaaSubpixel, 0.f, 1.f);
        fxaaEdgeThreshold = std::clamp(fxaaEdgeThreshold, 0.03f, 0.35f);
        grassMinimumPixels = std::clamp(grassMinimumPixels, 0.25f, 4.f);
        msaaSamples = msaaSamples >= 8u ? 8u : msaaSamples >= 4u ? 4u : msaaSamples >= 2u ? 2u : 1u;
        taaFeedback = std::clamp(taaFeedback, 0.75f, 0.97f);
        taaSharpen = std::clamp(taaSharpen, 0.f, 1.f);
    }
};

// Master switches and scales over each scene's look settings. A switch that
// is off disables the effect in every scene; scales multiply the scene's own
// values, so 1 leaves scenes as authored.
struct PostEffectsSettings
{
    bool bloom = true;
    float bloomScale = 1.f;
    bool autoExposure = true;
    // Added to every scene's output exposure, in EV (stops).
    float exposureBias = 0.f;
    bool colorGrading = true;
    // Scales grading contrast and saturation toward neutral (0) or beyond (>1).
    float gradingStrength = 1.f;
    bool ambientOcclusion = true;

    void Clamp()
    {
        bloomScale = std::clamp(bloomScale, 0.f, 4.f);
        exposureBias = std::clamp(exposureBias, -6.f, 6.f);
        gradingStrength = std::clamp(gradingStrength, 0.f, 2.f);
    }
};

struct RenderingEffectsSettings
{
    AntiAliasingSettings antiAliasing;
    PostEffectsSettings postEffects;

    void Clamp()
    {
        antiAliasing.Clamp();
        postEffects.Clamp();
    }
};
}
