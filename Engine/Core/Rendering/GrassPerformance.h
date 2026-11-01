#pragma once
#include <cstdint>

namespace Engine::Rendering
{
struct GrassPerformance
{
    double totalMs = 0, scanMs = 0, generationMs = 0, uploadMs = 0;
    uint64_t placementsScanned = 0, tilesBuilt = 0, lodSwitches = 0;
    uint64_t pending = 0, residentLods = 0, gpuBytes = 0;
    uint64_t instances = 0, instanceUploadBytes = 0;
    uint64_t activeTiles=0, transitionTiles=0, fadingTiles=0;
};
}
