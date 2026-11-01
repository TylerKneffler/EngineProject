#include "Core/View/Views/PerformanceRecording.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include "Diagnostics/PerformanceMetrics.h"
#include "Core/Rendering/RenderPerformance.h"
#include <limits>

int main()
{
    const auto stats=Engine::Editor::SummarizeFrames({10,20,30,40,0,-1,std::numeric_limits<double>::quiet_NaN()});
    if(stats.meanMs!=25 || stats.fps!=40 || stats.p95Ms!=40 || stats.p99Ms!=40 || stats.over60BudgetPercent!=75)
        throw std::runtime_error("Frame statistics must use mean intervals and ignore invalid samples");
    Engine::Rendering::RenderCpuTimings view, all;
    view.totalMs=3; view.views=1; view.physicalDraws=4;
    all+=view; all+=view;
    if(all.totalMs!=6 || all.views!=2 || all.physicalDraws!=8)
        throw std::runtime_error("Rendering measurements did not aggregate multiple views");
    const auto path = std::filesystem::current_path() /
        "engine-performance-recording-test.jsonl";
    Engine::Editor::PerformanceRecording recording;
    if (!recording.Start(path)) throw std::runtime_error("recording did not start");
    recording.Event("console", "Error", "quoted \"message\"\nnext line");
    recording.Frame(120.f, 8.5, 1.25);
    recording.FrameDetails("\"playing\":true,\"update_ms\":2.5,\"gpu_valid\":false,\"game_view_width\":1920");
    recording.FrameDetails("\"playing\":false,\"update_ms\":0,\"gpu_valid\":true,\"game_view_width\":1280");
    recording.Sample("\"cpu_user_pct\":25,\"gpu_engines_pct\":[12,0,0,0,0]");
    std::cout << "engine stdout line\n";
    std::cerr << "engine stderr line\n";
    recording.Stop();
    if (recording.IsRecording()) throw std::runtime_error("recording did not stop");
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    input.close();
    for (const char* expected : { "quoted \\\"message\\\"\\nnext line",
        "\"type\":\"frame\"", "\"frame_ms\":8.500",
        "\"panel_draw_ms\":1.250",
        "\"type\":\"frame_details\"",
        "\"frame\":0,\"playing\":true,\"update_ms\":2.5,\"gpu_valid\":false,\"game_view_width\":1920",
        "\"frame\":1,\"playing\":false,\"update_ms\":0,\"gpu_valid\":true,\"game_view_width\":1280",
        "\"cpu_user_pct\":25", "engine stdout line",
        "engine stderr line" })
        if (contents.find(expected) == std::string::npos)
        {
            std::cerr << "Missing recording field: " << expected << '\n'
                << contents << '\n';
            return 1;
        }
    std::filesystem::remove(path);
}
