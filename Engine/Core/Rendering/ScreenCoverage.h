#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Engine::Rendering
{
// Screen coverage drives detail.
//
// What an object shows on screen grows with its size and the camera's focal
// length in pixels, and shrinks with distance:
//     projected size ~ size * focalPixels / distance
// LOD distances are authored for a reference camera (ReferenceLodFovDegrees
// vertical field of view, ReferenceLodViewportPixels tall) and an object at
// its authored scale. Comparing
//     distance / (ViewLodCoverageScale * PlacementLodScale)
// against those distances therefore selects LODs by screen coverage: a
// zoomed-in or high-resolution view keeps detail further out, a scaled-up
// placement keeps it as far as its larger silhouette needs, and small or far
// objects drop detail first. At the reference camera nothing changes.
//
// ENGINE_LOD_SCREEN_COVERAGE=0 restores plain distances (for A/B captures).
constexpr float ReferenceLodFovDegrees = 60.f;
constexpr float ReferenceLodViewportPixels = 1080.f;
// Clamp so extreme zoom, tiny viewports or odd placement scales cannot push
// LOD distances to absurd values. Must match InstanceCull.hlsl/Object.hlsl.
constexpr float MinimumLodCoverageScale = .25f;
constexpr float MaximumLodCoverageScale = 4.f;

// Fair visibility (on by default; ENGINE_FAIR_LOD=0 turns it off for A/B
// runs): the level every object draws at depends only on its world distance
// from the viewer (and its own placement scale), never on the view's zoom,
// resolution, graphics quality or the detail budget. Levels differ in how
// much they let through (a far tree's crown, a hill's silhouette), so two
// players looking at each other through the same tree, zoomed in or not, on
// any machine, see the same levels (.md/todo.md, fair visibility).
inline bool FairVisibilityLod()
{
    static const bool enabled = []
    {
        const char* value = std::getenv("ENGINE_FAIR_LOD");
        return !(value && std::strcmp(value, "0") == 0);
    }();
    return enabled;
}

// Projected size in pixels of a sphere of this diameter at this distance,
// seen by the reference camera (fair visibility's stand-in for a view's own
// projected size).
inline float ReferenceLodPixels(float diameter, float distance)
{
    const float focal = ReferenceLodViewportPixels * .5f /
        std::tan(ReferenceLodFovDegrees * .5f * 3.14159265f / 180.f);
    return diameter * focal / std::max(distance, .01f);
}

inline bool ScreenCoverageLodEnabled()
{
    static const bool enabled = []
    {
        const char* value = std::getenv("ENGINE_LOD_SCREEN_COVERAGE");
        return !(value && std::strcmp(value, "0") == 0);
    }();
    return enabled;
}

// Focal length in pixels of this view relative to the reference camera.
// projectionYScale is the projection matrix's [1][1] term (1 / tan(fov / 2)
// for a perspective lens, including any zoom). Orthographic views have no
// perspective shrink, so distance LODs keep their authored meaning there.
inline float ViewLodCoverageScale(float projectionYScale, float viewportHeight,
    bool orthographic)
{
    if (FairVisibilityLod() || !ScreenCoverageLodEnabled() || orthographic ||
        !(viewportHeight > 0.f) || !(projectionYScale > 0.f) ||
        !std::isfinite(projectionYScale))
        return 1.f;
    const float referenceHalfFov = glm::radians(ReferenceLodFovDegrees) * .5f;
    const float focal = viewportHeight * .5f * projectionYScale;
    const float reference = ReferenceLodViewportPixels * .5f / std::tan(referenceHalfFov);
    const float scale = focal / reference;
    return std::isfinite(scale)
        ? std::clamp(scale, MinimumLodCoverageScale, MaximumLodCoverageScale) : 1.f;
}

// Size of one instanced placement relative to its source mesh (placement rows
// describe a uniform-scale affine transform).
inline float PlacementLodScale(const glm::vec4& firstRow)
{
    const float scale = glm::length(glm::vec3(firstRow));
    return std::isfinite(scale) && scale > 0.f
        ? std::clamp(scale, MinimumLodCoverageScale, MaximumLodCoverageScale) : 1.f;
}

// Fraction of the viewport covered by a projected box's screen rectangle,
// 0 when wholly outside the view and 1 when it crosses the near plane.
inline float ProjectedScreenCoverage(const glm::vec4 (&clips)[8])
{
    glm::vec2 lo(1.f), hi(-1.f);
    for (const auto& clip : clips)
    {
        if (!(clip.w > 1e-5f) || !std::isfinite(clip.x + clip.y + clip.w)) return 1.f;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        lo = glm::min(lo, ndc); hi = glm::max(hi, ndc);
    }
    lo = glm::clamp(lo, glm::vec2(-1.f), glm::vec2(1.f));
    hi = glm::clamp(hi, glm::vec2(-1.f), glm::vec2(1.f));
    const glm::vec2 extent = glm::max(hi - lo, glm::vec2(0.f));
    return extent.x * extent.y * .25f;
}
}
