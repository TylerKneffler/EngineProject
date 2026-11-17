#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    // Synthetic mouse-look: cursor pixels right of the look anchor per
    // measured frame (yaw while looking around); 0 disables.
    int lookYawPixels = 0;
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
    // Fixed viewpoint for repeatable vista captures: moves the game camera's
    // object to cameraPosition and aims it at cameraTarget (world space)
    // before the scene starts. Either may be given alone.
    std::optional<std::array<double, 3>> cameraPosition;
    std::optional<std::array<double, 3>> cameraTarget;
    // Called on the render thread as measured frames begin and after the
    // last one (e.g. to bracket an external CPU profiler).
    std::function<void()> onMeasurementStart;
    std::function<void()> onMeasurementEnd;
};

class GameApplication
{
public:
    static int Run(HINSTANCE instance, const GameBenchmarkOptions* benchmark = nullptr);
};
}
