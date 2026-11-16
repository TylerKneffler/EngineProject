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
    if (!ScreenCoverageLodEnabled() || orthographic ||
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
