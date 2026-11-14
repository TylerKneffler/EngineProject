#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Graphics
{
class IGraphicsProvider;
class IGraphicsTexture;
}

namespace Engine::Components
{
class Material;

// Packs the painted layers of a terrain into three Texture2DArrays, one slice
// per layer, so the terrain shader indexes layers dynamically instead of
// binding a texture per layer and map:
//   albedoHeight: rgb base color (sRGB), a = height (white = raised)  BC7
//   normal:       rg tangent normal XY                                 BC5
//   roughAo:      r roughness, g ambient occlusion                     BC5
// Devices without BC sampling get RGBA8 arrays at a smaller cap. Every slice
// shares one size (the largest layer map, capped). Maps a layer lacks are
// filled with neutral values: flat normal, roughness 1, AO 1, height 0.5.
// Arrays are built and BC-encoded on worker threads, cached on disk (temp
// directory, keyed by source sizes and write times) and in memory by the
// layers' texture paths, so cells that share materials share one set.
class TerrainLayerArrays
{
public:
    static constexpr uint32_t MaxLayers = 6;
    // Six 2K BC slices take ~100 MiB for all three arrays; the RGBA8
    // fallback stops at 1K (~100 MiB as well).
    static constexpr uint32_t MaxSliceSizeCompressed = 2048;
    static constexpr uint32_t MaxSliceSize = 1024;

    struct Arrays
    {
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> albedoHeight;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> normal;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> roughAo;
        // Linear mean albedo of each slice (its 1x1 mip), for macro color.
        std::array<glm::vec3, MaxLayers> meanAlbedo{};
        // Bit per layer: slice holds albedo / normal / roughness or AO / height.
        uint32_t albedoMask = 0, normalMask = 0, roughnessMask = 0, heightMask = 0;
        uint32_t sliceSize = 0;
    };

    // Ready arrays for these layer materials, or nullptr while they are
    // still being built (the first call starts the build). Call on the
    // graphics owner thread; GPU creation happens here.
    static const Arrays* Acquire(
        const std::vector<std::shared_ptr<Material>>& materials,
        Engine::Graphics::IGraphicsProvider* provider);
    // Drops cached arrays (scene unload, device loss).
    static void Clear();
};
}
