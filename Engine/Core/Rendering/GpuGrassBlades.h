#pragma once

#include <cstdint>
#include <memory>

namespace Engine::Graphics { class IGraphicsBuffer; }

namespace Engine::Rendering
{
// GPU-generated grass blades (.md/todo.md "Next" 2). A carrier Mesh (one blade
// template, one dummy placement, Mesh::gpuBladeField) draws through the
// ordinary instanced batch path, but its visible list is written by the blade
// generation compute pass (Shaders/GrassBlades/GrassBlades.hlsl) instead of
// the placement cull: blades come from a deterministic hash of position, the
// terrain's height and paint and the meadow's density, every frame, with no
// CPU placements. The owner (the grass feature) keeps `data` current; its
// layout (header, cell table, per-cell height and paint grids) is documented
// in GrassBlades.hlsl. The material's shader (Assets/Shaders/GrassBlades.hlsl)
// builds each blade from its record.
struct GpuBladeField
{
    std::shared_ptr<Engine::Graphics::IGraphicsBuffer> data;
    // Blades the visible list can hold (more are dropped).
    uint32_t capacity = 0;
    // Patches dispatched around the camera: side length (m) and reach (m).
    float patchSize = 2.f;
    float reach = 60.f;
    // Set by the renderer when it cannot draw blades (no indirect draws or
    // instance culling, or the generation shader failed to build); the owner
    // then falls back to mesh grass.
    bool unavailable = false;
};

// Compute constants (128-byte root constants; GrassBlades.hlsl BladeConstants).
struct GpuBladeConstants
{
    float viewProjection[16];
    float camera[4];      // xyz camera position, w reach
    int32_t grid[4];      // patch origin x, z (in patches), patches per side, mode (0 generate, 1 reset)
    uint32_t draw[4];     // object slot, index count, capacity, flags (1 occlusion)
};
static_assert(sizeof(GpuBladeConstants) <= 128);
}
