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
    // Two extra snapshots taken while the camera is still being driven
    // (--drive-forward), this many frames apart, the second two frames before
    // the drive ends: capture_a.ppm and capture_b.ppm. Things that come into
    // being between them are pop-in. 0 takes none.
    uint32_t movingCaptureGap = 0;
    // Also capture seq_<n>.ppm every captureEvery frames of the drive (0 =
    // off), to see streaming hand-offs while moving; without a drive, of the
    // measured frames (a still camera, for flicker).
    uint32_t captureEvery = 0;
    // Sequence captures without waiting for the GPU (async readback ring,
    // half resolution, written on a thread), so frames stay in flight and
    // the run keeps its real speed. DX12 only; others fall back to waiting.
    bool captureAsync = false;
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
    // Name of a scene object with a Camera (usually inactive, e.g. "Vista")
    // whose pose the game camera takes before the scene starts; applied
    // before cameraPosition/cameraTarget, which can still refine it.
    std::string viewpoint;
    // Name of a scene object with a SplinePath (e.g. "ForestTrailWalk") the
    // game camera walks instead of holding W: it starts at the route's first
    // point (warm-up included) and, for driveForwardFrames measured frames
    // from the frame W would go down, moves routeSpeed / 60 metres per frame,
    // looking at the route routeLookAhead metres on. Fixed steps per frame,
    // not per second, so every run renders the same poses (0 frames: walks
    // until the run ends). Replaces viewpoint / cameraPosition /
    // cameraTarget; past the end it stays there.
    std::string route;
    float routeSpeed = 6.f;
    float routeLookAhead = 12.f;
    // Route walks: the gaze swings left and right by this many degrees (a
    // sine over 240 frames, 4 s at 60 fps), so things beside the route pass
    // through the screen edges (pops and occlusion misses there). Per frame,
    // so every run renders the same poses. 0 looks straight along the route.
    float routeLookSway = 0.f;
    // A "Flag flicker (F8)" button in the corner: pressing F8 or clicking it
    // writes {"type":"flag","frame":N} into frames.jsonl (N = the measured
    // frame it was pressed on; the flicker came a moment before). Normal
    // play: ENGINE_FLICKER_BUTTON=1, flags go to .local/flicker-flags.txt.
    bool flagButton = false;
    // Frames per sway cycle (default 240). Short cycles with a wide sway turn
    // like a player glancing away and back (e.g. 120 degrees over 80 frames
    // peaks near 9 degrees a frame).
    uint32_t routeLookPeriod = 240;
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
