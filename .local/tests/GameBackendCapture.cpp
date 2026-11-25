#include "Game/Application/GameApplication.h"
#include "CpuSampler.h"
#include "Core/Startup/LaunchActivation.h"
#include <memory>
#pragma comment(lib, "winmm.lib")
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
uint32_t Number(const char* text, const char* name, uint32_t maximum, bool allowZero = false)
{
    const std::string value(text);
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string("invalid ") + name);
    size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed > maximum || (!allowZero && parsed == 0))
        throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<uint32_t>(parsed);
}

std::array<double, 3> Vector3(const char* text, const char* name)
{
    std::array<double, 3> result{};
    std::string value(text);
    for (char& c : value) if (c == ',') c = ' ';
    char* cursor = value.data();
    for (double& component : result)
    {
        char* end = nullptr;
        component = std::strtod(cursor, &end);
        if (end == cursor || !std::isfinite(component))
            throw std::runtime_error(std::string("invalid ") + name + " (expected x,y,z)");
        cursor = end;
    }
    while (*cursor == ' ') ++cursor;
    if (*cursor) throw std::runtime_error(std::string("invalid ") + name + " (expected x,y,z)");
    return result;
}

float Metres(const char* text, const char* name)
{
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end || !std::isfinite(value) || value <= 0.0 || value > 1000.0)
        throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<float>(value);
}

// --cpu-sample: in-process sampling profile of the measured frames, written
// to <out>/cpu-profile.txt after the run.
bool g_cpuSample = false;
bool g_foreground = false;
// --hang-report <s>: sample from startup; if measurement has not started after
// <s> seconds, write <out>/hang-profile.txt and exit (diagnoses load stalls).
int g_hangReport = 0;

Engine::Game::GameBenchmarkOptions Parse(int argc, char** argv)
{
    Engine::Game::GameBenchmarkOptions options;
    options.scene = std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes/terrain/landscape_demo.scene";
    options.output = ".local/performance-logs";
    for (int i = 1; i < argc; ++i)
    {
        const std::string key = argv[i];
        const auto value = [&]() {
            if (++i >= argc) throw std::runtime_error("missing value after " + key);
            return argv[i];
        };
        if (key == "--scene") options.scene = value();
        else if (key == "--project") options.project = value();
        else if (key == "--backend") options.backend = value();
        else if (key == "--device") options.device = value();
        else if (key == "--out") options.output = value();
        else if (key == "--width") options.width = Number(value(), "width", 16384);
        else if (key == "--height") options.height = Number(value(), "height", 16384);
        else if (key == "--warmup") options.warmup = Number(value(), "warmup", 100000, true);
        else if (key == "--frames") options.frames = Number(value(), "frames", 100000);
        else if (key == "--wait-for-residency") options.residencyTimeoutSeconds =
            Number(value(), "residency timeout seconds", 3600);
        else if (key == "--drive-forward") options.driveForwardFrames =
            Number(value(), "forward input frames", 100000);
        else if (key == "--drive-boost") options.driveBoost = true;
        else if (key == "--moving-captures") options.movingCaptureGap =
            Number(value(), "frames between the two moving snapshots", 1000);
        else if (key == "--capture-every") options.captureEvery =
            Number(value(), "frames between drive snapshots", 10000);
        else if (key == "--capture-burst") options.captureBurst =
            Number(value(), "consecutive frames per drive snapshot", 10000);
        else if (key == "--render-mode") options.renderMode =
            static_cast<int>(Number(value(), "render mode", 6, true));
        else if (key == "--max-fps") options.maxFpsOverride = Number(value(), "max fps", 1000, true);
        else if (key == "--profile") options.profile = value();
        else if (key == "--require-terrain") options.requireTerrain = true;
        else if (key == "--require-streamed-cells") options.requireStreamedCells = true;
        else if (key == "--require-movement") options.requireMovement = true;
        else if (key == "--look-yaw") options.lookYawPixels =
            static_cast<int>(Number(value(), "look yaw pixels", 2000, true));
        else if (key == "--disable-clustered-lighting") options.disableClusteredLighting = true;
        else if (key == "--camera-position") options.cameraPosition = Vector3(value(), "camera position");
        else if (key == "--camera-target") options.cameraTarget = Vector3(value(), "camera target");
        else if (key == "--viewpoint") options.viewpoint = value();
        else if (key == "--route") options.route = value();
        else if (key == "--route-speed") options.routeSpeed = Metres(value(), "route speed");
        else if (key == "--route-look-ahead") options.routeLookAhead = Metres(value(), "route look-ahead");
        else if (key == "--capture-async") options.captureAsync = true;
        else if (key == "--flag-button") options.flagButton = true;
        else if (key == "--route-look-sway") options.routeLookSway = Metres(value(), "route look sway");
        else if (key == "--route-look-period") options.routeLookPeriod = Number(value(), "route look period frames", 100000);
        else if (key == "--cpu-sample") g_cpuSample = true;
        else if (key == "--foreground") g_foreground = true;
        else if (key == "--hang-report") g_hangReport = static_cast<int>(Number(value(), "hang report seconds", 3600));
        else if (key == "--help" || key == "-h")
        {
            std::cout << "GameBackendCapture [--scene <file.scene>] [--project <file.proj>] "
                "[--backend dx11|dx12|vulkan] [--device <adapter>] [--width 1920 --height 1080] "
                "[--warmup 120 --frames 300 --out .local/performance-logs] "
                "[--require-terrain] [--require-streamed-cells] [--wait-for-residency <seconds>] "
                "[--drive-forward <frames>] [--look-yaw <pixels/frame>] [--require-movement] [--max-fps <fps>] [--profile <video profile>] "
                "[--disable-clustered-lighting] [--viewpoint <camera object>] [--camera-position x,y,z] [--camera-target x,y,z] "
                "[--route <SplinePath object> [--route-speed <m/s, default 6>] [--route-look-ahead <m, default 12>]] "
                "[--cpu-sample] [--foreground]\n"
                "--route walks the game camera along a scene path (landscape demo: ForestTrailWalk, MeadowTrailWalk) "
                "at a fixed routeSpeed/60 m per frame for --drive-forward frames (default: all measured frames but eight).\n"
                "Defaults to terrain/landscape_demo.scene. Runs the standalone Game loop, writes frames.jsonl and capture.ppm, "
                "and returns 2 if capture validity checks fail. Windows open without focus, behind the active app, "
                "unless --foreground is given.\n";
            std::exit(0);
        }
        else throw std::runtime_error("unknown option: " + key);
    }
    options.scene = std::filesystem::absolute(options.scene);
    if (!std::filesystem::is_regular_file(options.scene))
        throw std::runtime_error("scene file not found: " + options.scene.string());
    if (!options.project.empty())
    {
        options.project = std::filesystem::absolute(options.project);
        if (!std::filesystem::is_regular_file(options.project))
            throw std::runtime_error("project file not found: " + options.project.string());
    }
    if (options.backend != "dx11" && options.backend != "dx12" && options.backend != "vulkan")
        throw std::runtime_error("backend must be dx11, dx12, or vulkan");
    if (!options.route.empty())
    {
        if (options.lookYawPixels || options.driveBoost || options.cameraPosition ||
            options.cameraTarget || !options.viewpoint.empty())
            throw std::runtime_error("--route sets the camera itself: no --look-yaw, --drive-boost, "
                "--viewpoint, --camera-position or --camera-target with it");
        // Walk for the whole measured run unless told how long.
        if (!options.driveForwardFrames)
        {
            if (options.frames <= 8u)
                throw std::runtime_error("--route needs more than eight measured frames");
            options.driveForwardFrames = options.frames - 8u;
        }
    }
    if (options.driveForwardFrames && options.frames < options.driveForwardFrames + 8u)
        throw std::runtime_error("--frames must include the forward input and eight setup frames");
    return options;
}
}

int main(int argc, char** argv)
{
    try
    {
        auto options = Parse(argc, argv);
        // Captures run while the user keeps working: never take focus.
        Engine::Core::SetBackgroundLaunch(!g_foreground);
        // ...and keep full speed while unfocused, or Windows moves the
        // process to efficiency cores and the timings measure that instead.
        Engine::Core::DisablePowerThrottling();
        std::unique_ptr<CpuSampler> sampler;
        if (g_hangReport)
        {
            static auto* hangSampler = new CpuSampler();
            static std::atomic<bool> started{false};
            hangSampler->Begin();
            options.onMeasurementStart = [] { started = true; };
            const auto out = options.output;
            std::thread([out] {
                std::this_thread::sleep_for(std::chrono::seconds(g_hangReport));
                if (started) return;
                hangSampler->Stop();
                std::filesystem::create_directories(out);
                hangSampler->Report(out / "hang-profile.txt", 4, 60);
                std::cout << "Hang profile: " << std::filesystem::absolute(out / "hang-profile.txt").string() << std::endl;
                TerminateProcess(GetCurrentProcess(), 3);
            }).detach();
        }
        else if (g_cpuSample)
        {
            sampler = std::make_unique<CpuSampler>();
            options.onMeasurementStart = [&sampler] { sampler->Begin(); };
            options.onMeasurementEnd = [&sampler] { sampler->End(); };
        }
        const int result = Engine::Game::GameApplication::Run(GetModuleHandleW(nullptr), &options);
        if (sampler)
        {
            std::filesystem::create_directories(options.output);
            const auto report = options.output / "cpu-profile.txt";
            sampler->Report(report);
            std::cout << "CPU profile: " << std::filesystem::absolute(report).string() << std::endl;
        }
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Game benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
