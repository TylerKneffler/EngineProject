// Command-line front end of the landscape generator (LandscapeGenerator.h),
// the same the editor's Tools > Terrain generator runs.
//
// Usage: LandscapeGenerate --heights <world.heights>
//          [--objects <world.objects>] [--cells <cellDataDirectory>]
//          [--extent 94] [--fine 23] [--seed 0] [--relief 1] [--erosion 0.2]
//          [--water -40] [--no-lakes] [--no-hills] [--no-escarpment]

#include "Core/Components/Terrain/LandscapeGenerator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    using namespace Engine::Components;
    LandscapeGeneratorSettings settings;
    LandscapeGeneratorPaths paths;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--heights") paths.heights = next();
        else if (arg == "--objects") paths.objectArchive = next();
        else if (arg == "--cells") paths.cellData = next();
        else if (arg == "--extent") settings.halfExtentCells = std::atoi(next());
        else if (arg == "--fine") settings.fineHalfExtentCells = std::atoi(next());
        else if (arg == "--seed") settings.seed = std::atoi(next());
        else if (arg == "--relief") settings.relief = float(std::atof(next()));
        else if (arg == "--erosion") settings.erosion = float(std::atof(next()));
        else if (arg == "--water") settings.waterLevel = float(std::atof(next()));
        else if (arg == "--no-lakes") settings.lakes = false;
        else if (arg == "--no-hills") settings.hills = false;
        else if (arg == "--no-escarpment") settings.escarpment = false;
        else { std::fprintf(stderr, "unknown option %s\n", arg.c_str()); return 2; }
    }
    if (paths.heights.empty())
    {
        std::fprintf(stderr, "LandscapeGenerate --heights <file> [options]\n");
        return 2;
    }
    std::string error;
    LandscapeGeneratorReport report;
    const char* last = "";
    const bool ok = GenerateLandscape(settings, paths, error,
        [&](float fraction, const char* stage) {
            if (std::strcmp(stage, last) != 0) { std::printf("%3.0f %% %s\n", fraction * 100.f, stage); last = stage; }
        }, nullptr, &report);
    if (!ok) { std::fprintf(stderr, "failed: %s\n", error.c_str()); return 1; }
    std::printf("heights %.0f..%.0f m, %zu cells changed; re-seated %zu sculpts, %zu placements, %zu object files, "
        "%zu volume edit files, %zu foliage files re-seated, %.1f s\n", report.lowest, report.highest,
        report.changedCells, report.sculptsMoved, report.placementsMoved, report.objectFilesMoved,
        report.volumeEditsMoved, report.foliageCellsMoved, report.seconds);
    return 0;
}
