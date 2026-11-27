#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <glm/glm.hpp>

namespace Engine::Rendering
{
// Cross-fade between a streamed cell's own objects (near) and the far
// impostor blocks (far) that drew it before (TerrainManager::UpdateFarObjects,
// .md/todo.md F5). A cell's handover progress p runs 0 -> 1 over
// SceneSettings::lodTransitionSeconds once its trees are ready: near
// placements in that cell dissolve in over the far impostors, which then
// dissolve out (Object.hlsl HandoverProgress, ApplyGeometryFade). Published
// once per update by the terrain manager for its scene; draws whose mesh has
// a handoverRole carry it in ObjectGPUData (viewTextureParams.yzw,
// handoverCells).
//
// A grid, not a list: Radius cells either side of the camera's cell, every
// cell that can be mid-handover (they are all within the loaded radius plus
// the unload margin). A list of 12, then 24, overflowed when flying (61
// cells wanted at 250 m/s): a dropped cell's near objects jumped to fully
// shown and back, so trees and bushes disappeared and reappeared.
struct CellHandover
{
    static constexpr int Radius = 5;
    static constexpr int Side = 2 * Radius + 1;              // 11 x 11 cells: 121 x 3 bits = 363 of 384
    static constexpr uint32_t Unlisted = 7u;                 // 3 bits per cell
    const void* scene = nullptr;
    // World x, z of the camera cell's corner, and 1 / cell span (square cells).
    glm::vec3 grid{0.f};
    // 3 bits per cell, row-major from (-Radius, -Radius), 10 cells per uint:
    // 7 not listed (near placements fully shown, p = 1; far ones too, p = 0),
    // else p * 6 (the screen dither and TAA smooth the seven steps).
    std::array<glm::uvec4, 3> cells{};

    static CellHandover& Get()
    {
        static CellHandover handover;
        return handover;
    }
    void Clear() { for (auto& v : cells) v = glm::uvec4(0xffffffffu); }
    // False when the cell lies outside the grid.
    bool Set(int dx, int dz, float progress)
    {
        if (std::abs(dx) > Radius || std::abs(dz) > Radius) return false;
        const uint32_t index = static_cast<uint32_t>((dz + Radius) * Side + dx + Radius);
        const uint32_t value = static_cast<uint32_t>(std::lround(std::clamp(progress, 0.f, 1.f) * 6.f));
        uint32_t& word = cells[index / 40][(index / 10) % 4];
        const uint32_t shift = (index % 10) * 3u;
        word = (word & ~(7u << shift)) | (value << shift);
        return true;
    }
};
}
