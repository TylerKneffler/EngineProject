#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <glm/vec4.hpp>

namespace Engine::Rendering::Portal
{
enum class Backend : uint8_t
{
    DirectX11,
    DirectX12,
    Vulkan
};

enum class ApertureWriteOperation : uint8_t
{
    Replace,
    IncrementAndClamp
};

struct StencilStep
{
    ApertureWriteOperation writeOperation = ApertureWriteOperation::Replace;
    uint32_t writeReference = 1;
    uint32_t sceneReadReference = 1;

    constexpr bool operator==(const StencilStep& other) const
    {
        return writeOperation == other.writeOperation &&
            writeReference == other.writeReference &&
            sceneReadReference == other.sceneReadReference;
    }
};

inline constexpr int kMinimumRecursionDepth = 1;
inline constexpr int kMaximumRecursionDepth = 8;
// A value of one permits the direct source-to-target view only. Two permits
// source -> target -> source once, and higher values extend that linked-pair
// loop, subject to the independent total-depth and per-frame view budgets.
inline constexpr int kMinimumConnectionRepeatLimit = 1;
inline constexpr int kMaximumConnectionRepeatLimit = 8;
inline constexpr int kMinimumViewBudget = 1;
inline constexpr int kMaximumViewBudget = 64;

constexpr int ClampRecursionDepth(int requested)
{
    return std::clamp(requested, kMinimumRecursionDepth,
        kMaximumRecursionDepth);
}

constexpr int ClampConnectionRepeatLimit(int requested)
{
    return std::clamp(requested, kMinimumConnectionRepeatLimit,
        kMaximumConnectionRepeatLimit);
}

constexpr bool CanRepeatConnection(std::size_t priorVisits, int requestedLimit)
{
    return priorVisits < static_cast<std::size_t>(
        ClampConnectionRepeatLimit(requestedLimit));
}

// After crossing an entrance, its connected endpoint is the exit plane behind
// the virtual camera boundary. It must not immediately be treated as another
// entrance; doing so lets aligned equal-size endpoints cover the entire parent
// view instead of revealing the portal visible across the connected space.
constexpr bool IsImmediateExitAperture(
    const void* candidate, const void* previousExit)
{
    return previousExit != nullptr && candidate == previousExit;
}

// Ordinary objects have no chart owner and are visible through every portal.
// A split traversal instance already exists once in each endpoint's chart;
// connected rendering must select the destination instance instead of mapping
// the source instance on top of it and reconstructing the full object there.
constexpr bool IsTraversalInstanceVisibleInConnectedChart(
    const void* instanceChart, const void* destinationChart)
{
    return instanceChart == nullptr || instanceChart == destinationChart;
}

constexpr int ClampViewBudget(int requested)
{
    return std::clamp(requested, kMinimumViewBudget, kMaximumViewBudget);
}

constexpr uint32_t TriangulatedApertureVertexCount(std::size_t pointCount)
{
    return pointCount < 3
        ? 0u
        : static_cast<uint32_t>((pointCount - 2u) * 3u);
}

// Clip an ordered convex aperture polygon in homogeneous clip space. Testing
// only vertices whose w is positive makes a portal disappear discontinuously
// when the eye/near plane crosses it: the frustum can still intersect edges or
// lie inside a large aperture even when none of its original corners is in
// front. Clipping first gives visibility and scissor calculations the actual
// on-screen polygon. Camera projections in the engine use the ZO convention.
inline std::vector<glm::vec4> ClipApertureToViewFrustum(
    std::vector<glm::vec4> polygon)
{
    const auto clipAgainst = [](std::vector<glm::vec4> input,
                                const auto& signedDistance)
    {
        std::vector<glm::vec4> output;
        if (input.empty())
            return output;
        output.reserve(input.size() + 2u);
        glm::vec4 previous = input.back();
        float previousDistance = signedDistance(previous);
        bool previousInside = previousDistance >= 0.f;
        for (const glm::vec4& current : input)
        {
            const float currentDistance = signedDistance(current);
            const bool currentInside = currentDistance >= 0.f;
            if (currentInside != previousInside)
            {
                const float denominator = previousDistance - currentDistance;
                const float t = denominator != 0.f
                    ? previousDistance / denominator : 0.f;
                output.push_back(previous + t * (current - previous));
            }
            if (currentInside)
                output.push_back(current);
            previous = current;
            previousDistance = currentDistance;
            previousInside = currentInside;
        }
        return output;
    };

    polygon = clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.x + p.w; });
    polygon = clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.w - p.x; });
    polygon = clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.y + p.w; });
    polygon = clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.w - p.y; });
    polygon = clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.z; });
    return clipAgainst(std::move(polygon),
        [](const glm::vec4& p) { return p.w - p.z; });
}

// Each top-level portal receives a disjoint stencil range. At the root, the
// aperture replaces with that range's base value. A nested aperture matches
// and increments its parent, keeping descendants clipped to every ancestor.
constexpr StencilStep StencilForDepth(
    Backend, uint32_t depth, uint32_t rootBase = 1u)
{
    return depth == 0u
        ? StencilStep { ApertureWriteOperation::Replace, rootBase, rootBase }
        : StencilStep { ApertureWriteOperation::IncrementAndClamp,
            rootBase + depth - 1u, rootBase + depth };
}
}
