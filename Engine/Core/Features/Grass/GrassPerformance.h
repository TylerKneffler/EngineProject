#pragma once
#include <cstdint>

namespace Engine::Features::Grass
{
// One frame's GrassField work, summed over every field in the scene
// (GrassStats). Read by the editor profiler and the game benchmark.
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

    GrassPerformance& operator+=(const GrassPerformance& value)
    {
        totalMs += value.totalMs; scanMs += value.scanMs;
        generationMs += value.generationMs; uploadMs += value.uploadMs;
        referencesMs += value.referencesMs; sourcesMs += value.sourcesMs;
        placementMs += value.placementMs; visibilityMs += value.visibilityMs;
        retirementMs += value.retirementMs; schedulingMs += value.schedulingMs;
        buildMs += value.buildMs; accountingMs += value.accountingMs;
        otherMs += value.otherMs;
        placementsScanned += value.placementsScanned; tilesBuilt += value.tilesBuilt;
        lodSwitches += value.lodSwitches; pending += value.pending;
        residentLods += value.residentLods; gpuBytes += value.gpuBytes;
        instances += value.instances; instanceUploadBytes += value.instanceUploadBytes;
        activeTiles += value.activeTiles; transitionTiles += value.transitionTiles;
        fadingTiles += value.fadingTiles;
        return *this;
    }
};
}
