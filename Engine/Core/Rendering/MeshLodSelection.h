#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Engine::Rendering
{
// Visible geometry policy only. Shadow selection must use its own policy/state.
struct MeshLodView
{
    glm::mat4 viewProjection{1.f}; // includes lens FOV, aspect and zoom
    glm::vec2 viewportPixels{0.f}; // actual render target size, after render scale
    bool zeroToOneDepth = false;
};

// Project the entire transformed box, without screen-edge clipping. Any box
// crossing the eye/near plane conservatively retains the finest geometry.
// Callers supply stable bounds enclosing ALL LODs and maximum animation/wind.
inline float ProjectedLodPixels(const glm::vec3& minimum,
    const glm::vec3& maximum, const glm::mat4& world, const MeshLodView& view)
{
    const float infinity = std::numeric_limits<float>::infinity();
    if (!(view.viewportPixels.x > 0.f && view.viewportPixels.y > 0.f)) return infinity;
    glm::vec2 lo(infinity), hi(-infinity);
    const auto matrix = view.viewProjection * world;
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec4 clip = matrix * glm::vec4(
            corner & 1 ? maximum.x : minimum.x,
            corner & 2 ? maximum.y : minimum.y,
            corner & 4 ? maximum.z : minimum.z, 1.f);
        if (!std::isfinite(clip.w) || clip.w <= 1e-5f ||
            !std::isfinite(clip.z) || clip.z <= (view.zeroToOneDepth ? 0.f : -clip.w)) return infinity;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) return infinity;
        lo = glm::min(lo, ndc); hi = glm::max(hi, ndc);
    }
    const glm::vec2 pixels = (hi - lo) * view.viewportPixels * .5f;
    return std::max(pixels.x, pixels.y);
}

// Descending pixel thresholds: e.g. {300, 80}. Fractional hysteresis is
// relative to each boundary; state belongs to one object and one camera.
inline int SelectScreenMeshLod(float pixels, const std::vector<float>& thresholds,
    int previous = -1, float hysteresis = .1f)
{
    if (std::isnan(pixels)) return std::max(0, previous);
    hysteresis = std::isfinite(hysteresis) ? std::clamp(hysteresis, 0.f, .49f) : 0.f;
    int target = 0;
    float upper = std::numeric_limits<float>::infinity();
    for (float boundary : thresholds)
    {
        // Invalid authoring fails towards quality rather than dropping geometry.
        if (!std::isfinite(boundary) || boundary <= 0.f || boundary >= upper) return 0;
        if (pixels < boundary) ++target;
        upper = boundary;
    }
    if (previous >= 0 && previous <= static_cast<int>(thresholds.size()))
    {
        const bool belowUpper = previous == 0 || pixels < thresholds[previous - 1] * (1.f + hysteresis);
        const bool aboveLower = previous == static_cast<int>(thresholds.size()) ||
            pixels >= thresholds[previous] * (1.f - hysteresis);
        if (belowUpper && aboveLower) return previous;
    }
    return target;
}
}
