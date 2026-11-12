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
    std::optional<uint32_t> maxFpsOverride;
    bool requireTerrain = false, requireStreamedCells = false, requireMovement = false;
    bool disableClusteredLighting = false;
};

class GameApplication
{
public:
    static int Run(HINSTANCE instance, const GameBenchmarkOptions* benchmark = nullptr);
};
}
