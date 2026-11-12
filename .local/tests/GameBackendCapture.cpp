#include "Game/Application/GameApplication.h"
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

Engine::Game::GameBenchmarkOptions Parse(int argc, char** argv)
{
    Engine::Game::GameBenchmarkOptions options;
    options.scene = std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes/terrain/oak_meadow.scene";
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
        else if (key == "--max-fps") options.maxFpsOverride = Number(value(), "max fps", 1000, true);
        else if (key == "--require-terrain") options.requireTerrain = true;
        else if (key == "--require-streamed-cells") options.requireStreamedCells = true;
        else if (key == "--require-movement") options.requireMovement = true;
        else if (key == "--disable-clustered-lighting") options.disableClusteredLighting = true;
        else if (key == "--help" || key == "-h")
        {
            std::cout << "GameBackendCapture [--scene <file.scene>] [--project <file.proj>] "
                "[--backend dx11|dx12|vulkan] [--device <adapter>] [--width 1920 --height 1080] "
                "[--warmup 120 --frames 300 --out .local/performance-logs] "
                "[--require-terrain] [--require-streamed-cells] [--wait-for-residency <seconds>] "
                "[--drive-forward <frames>] [--require-movement] [--max-fps <fps>] "
                "[--disable-clustered-lighting]\n"
                "Defaults to oak_meadow.scene. Runs the standalone Game loop, writes frames.jsonl and capture.ppm, "
                "and returns 2 if capture validity checks fail.\n";
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
    if (options.driveForwardFrames && options.frames < options.driveForwardFrames + 8u)
        throw std::runtime_error("--frames must include the forward input and eight setup frames");
    return options;
}
}

int main(int argc, char** argv)
{
    try
    {
        const auto options = Parse(argc, argv);
        return Engine::Game::GameApplication::Run(GetModuleHandleW(nullptr), &options);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Game benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
