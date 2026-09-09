#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

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
