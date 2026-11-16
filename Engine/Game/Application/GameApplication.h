#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Engine::Game
{
struct GameBenchmarkOptions
{
    std::filesystem::path scene;
    std::filesystem::path project;
    std::filesystem::path output;
    std::string backend = "dx11";
    std::string device;
    uint32_t width = 1920, height = 1080, warmup = 120, frames = 300;
    uint32_t residencyTimeoutSeconds = 0;
    uint32_t driveForwardFrames = 0;
    // Also hold Ctrl (sprint / flight boost) while driving forward.
    bool driveBoost = false;
    // Scene view render mode for the game camera (-1 normal, 3 Performance
    // heat map); diagnostics only.
    int renderMode = -1;
    std::optional<uint32_t> maxFpsOverride;
    bool requireTerrain = false, requireStreamedCells = false, requireMovement = false;
    bool disableClusteredLighting = false;
    // Video profile to apply (Preferences > Video Profiles); empty uses
    // ENGINE_VIDEO_PROFILE or the project's active profile. Its resolution
    // replaces width/height; an explicit maxFpsOverride beats its limit.
    std::string profile;
};

class GameApplication
{
public:
    static int Run(HINSTANCE instance, const GameBenchmarkOptions* benchmark = nullptr);
};
}
