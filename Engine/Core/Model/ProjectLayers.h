#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Model
{
// A project-wide object category, like Unity's layers. Every object belongs
// to one (Object::layer, an index into ProjectSettings::layers). Layers are
// for reference and filtering, and they drive the detail budget below.
struct ProjectLayer
{
    std::string name;
    // Constant multiplier on the LOD distances (and other detail distances)
    // of objects in this layer. Above 1 keeps detail farther out.
    float lodDistanceScale = 1.f;
    // How strongly the detail budget moves this layer: 0 exempts it (always
    // authored detail), 1 is normal, above 1 sheds and gains detail first.
    float budgetWeight = 1.f;
};

// Maximum number of project layers (an object stores its layer as an index).
constexpr size_t MaxProjectLayers = 32;

// Default layers of a new project. Index 0 is always "Default".
inline std::vector<ProjectLayer> DefaultProjectLayers()
{
    return {
        { "Default", 1.f, 1.f },
        { "Terrain", 1.f, .5f },
        { "Trees", 1.f, 1.f },
        { "Grass", 1.f, 1.25f },
        { "Props", 1.f, 1.f },
        { "Characters", 1.f, 0.f },
        { "Water", 1.f, 0.f },
        { "UI", 1.f, 0.f },
    };
}

// Adaptive detail. The renderer measures the GPU time of geometry passes
// (opaque, terrain, grass, trees, foliage depth, shadows) and the CPU time
// of recording the scene's render passes, and scales every layer's LOD
// distances so the tighter of the two stays near its budget: spare time
// pushes detail farther out, overload pulls it in.
struct DetailBudgetSettings
{
    bool enabled = true;
    // GPU milliseconds per frame the geometry passes may use.
    float geometryBudgetMs = 6.f;
    // CPU milliseconds per frame for recording the scene's render passes.
    float cpuRenderBudgetMs = 4.f;
    // Range of the global detail scale (1 = authored distances). The floor is
    // 1 (2026-11-23): at .6 the budget sat there on every landscape walk (its
    // geometry estimate is always over 6 ms) and pulled tree levels in to
    // 0.6x, so forest-edge trees at 20-40 m drew a level down, for no
    // measurable frame time (full 400 m trail walks, scale 1 against .6:
    // 13.05 / 12.77 ms forest, 12.71 / 12.98 ms meadow; the frame is CPU-bound).
    // The budget can still raise detail; a lower floor is a project setting.
    float minimumScale = 1.f;
    float maximumScale = 8.f;
    // Seconds for the scale to settle after a change in load.
    float responseSeconds = 1.5f;

    // Texture memory, modelled on GTA V's texture budget. Streamed textures
    // share one pool: the base size is always available, and the extended
    // budget adds a share of the GPU's spare video memory (what the OS grants
    // the process, minus everything that is not texture and a safety
    // reserve). Within the pool, textures covering the most screen pixels
    // keep their finest mips; when it is full the rest fall back to coarser
    // mips rather than disappear. Off keeps every texture fully resident.
    bool textureBudgetEnabled = true;
    // Base pool, MiB.
    float textureBudgetMiB = 1024.f;
    // 0..1: share of the spare video memory added on top of the base pool.
    float extendedTextureBudget = .5f;
};
}
