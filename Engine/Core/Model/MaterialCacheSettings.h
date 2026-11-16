#pragma once

#include <algorithm>
#include <cstdint>

namespace Engine::Model
{
// How the renderer caches prepared materials. Two layers:
//
//  - Render material cache (CPU): each material's packed GPU record and
//    texture slots are built once per frame per feature variant (normal
//    mapping, parallax, environment diffuse, reflections, subsurface) and
//    reused by every draw of that material in the frame.
//  - Material descriptor sets (Vulkan): one descriptor set per unique
//    texture/buffer binding, reused across frames in the same frame slot.
//    Sets come from pools of descriptorSetsPerPool; more pools are added on
//    demand. When a slot holds more than descriptorCacheLimit sets (mostly
//    stale bindings), its pools are reset and the sets rebuilt.
//
// Project settings supply the defaults. A player/user settings layer can
// override them at runtime through Engine::Rendering::MaterialCachePolicy.
struct MaterialCacheSettings
{
    static constexpr uint32_t MinimumSetsPerPool = 64;
    static constexpr uint32_t MaximumSetsPerPool = 4096;
    static constexpr uint32_t MinimumCacheLimit = 64;
    static constexpr uint32_t MaximumCacheLimit = 65536;

    // Off rebuilds every draw's material record (diagnostics/profiling).
    bool renderMaterialCache = true;
    // Descriptor sets each Vulkan material pool holds. Larger pools mean
    // fewer allocations on material-heavy frames at the cost of memory.
    uint32_t descriptorSetsPerPool = 512;
    // Cached descriptor sets per frame slot before the slot is reset.
    uint32_t descriptorCacheLimit = 384;

    void Normalize()
    {
        descriptorSetsPerPool = std::clamp(descriptorSetsPerPool,
            MinimumSetsPerPool, MaximumSetsPerPool);
        descriptorCacheLimit = std::clamp(descriptorCacheLimit,
            MinimumCacheLimit, MaximumCacheLimit);
    }

    bool operator==(const MaterialCacheSettings& other) const
    {
        return renderMaterialCache == other.renderMaterialCache &&
            descriptorSetsPerPool == other.descriptorSetsPerPool &&
            descriptorCacheLimit == other.descriptorCacheLimit;
    }
    bool operator!=(const MaterialCacheSettings& other) const { return !(*this == other); }
};
}
