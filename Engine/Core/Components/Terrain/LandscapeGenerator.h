#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace Engine::Components
{
// The map-scale terrain generator (the engine's port of
// tools/build_landscape_demo.py's heights): a lowland valley around the start
// ringed by ridged ranges, carved by droplet hydraulic erosion, with rolling
// hills, a rock escarpment and lake basins, at 16 m over the whole map and
// 4 m with fine detail over the play area. Writes the height archive
// (TerrainHeightArchive, version 3); far terrain reads its corner grid from
// it (TerrainHeightArchive::ReadCornerGrid), so there is no far grid file.
//
// Everything built on the previous heights is re-seated on the new ground,
// nothing removed: sculpt edits (.terrainedit, absolute heights per vertex)
// keep their offset from the ground, placed objects (the object archive and
// each cell's .objects file) and volume edits move up or down with the
// ground under them, painted foliage moves with it column by column; paint
// stays as it is. (The Python tool deleted the sculpts of changed cells; any
// change of erosion changes nearly every cell.) The cell data generation stamp
// is renewed, so a scene saved before does not restore the old files on load
// (TerrainManager::CellDataGeneration).
//
// Layout fixed by the landscape's TerrainManager: cells of 32 x 32 quads at
// 4 m (128 m), heights in 1/16 m, coarse cells every 4th sample.
struct LandscapeGeneratorSettings
{
    int halfExtentCells = 94;      // the map is (2n + 1) cells of 128 m a side
    int fineHalfExtentCells = 23;  // the 4 m play area (the rest at 16 m)
    int seed = 0;                  // 0: the demo map's noise; others vary it
    float relief = 1.f;            // height of the ranges (1 = up to ~1.9 km)
    float erosion = .2f;           // droplets per 16 m texel (0: none)
    float waterLevel = -40.f;      // lakes are carved below it
    bool lakes = true, hills = true, escarpment = true;
};

struct LandscapeGeneratorPaths
{
    std::string heights;        // world.heights
    std::string objectArchive;  // world.objects (re-seated; empty: none)
    std::string cellData;       // the manager's cellDataDirectory (edits, objects, foliage)
};

struct LandscapeGeneratorReport
{
    uint64_t signature = 0;
    float lowest = 0.f, highest = 0.f;
    size_t changedCells = 0, sculptsMoved = 0, placementsMoved = 0, objectFilesMoved = 0,
        volumeEditsMoved = 0, foliageCellsMoved = 0;
    double seconds = 0.0;
};

// fraction 0..1 and the stage running; called from the generating thread.
using LandscapeGeneratorProgress = std::function<void(float fraction, const char* stage)>;

// Generates and writes; runs for tens of seconds (call it off the main
// thread). cancel (optional) stops it before anything is written. False with
// `error` set on failure (nothing replaced).
bool GenerateLandscape(const LandscapeGeneratorSettings& settings, const LandscapeGeneratorPaths& paths,
    std::string& error, const LandscapeGeneratorProgress& progress = {},
    const std::atomic<bool>* cancel = nullptr, LandscapeGeneratorReport* report = nullptr);
}
