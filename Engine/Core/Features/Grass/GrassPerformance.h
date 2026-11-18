#pragma once
#include <cstdint>

namespace Engine::Rendering
{
struct GrassPerformance
{
    double totalMs = 0, scanMs = 0, generationMs = 0, uploadMs = 0;
    // Exclusive Refresh phases; scan/generation/upload are nested diagnostics.
    double referencesMs=0, sourcesMs=0, placementMs=0, visibilityMs=0;
    double retirementMs=0, schedulingMs=0, buildMs=0, accountingMs=0, otherMs=0;
    uint64_t placementsScanned = 0, tilesBuilt = 0, lodSwitches = 0;
    uint64_t pending = 0, residentLods = 0, gpuBytes = 0;
    uint64_t instances = 0, instanceUploadBytes = 0;
    uint64_t activeTiles=0, transitionTiles=0, fadingTiles=0;
};
}
