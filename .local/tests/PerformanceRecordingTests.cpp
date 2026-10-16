#include "Core/View/Views/PerformanceRecording.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int main()
{
    const auto path = std::filesystem::current_path() /
        "engine-performance-recording-test.jsonl";
    Engine::Editor::PerformanceRecording recording;
    if (!recording.Start(path)) throw std::runtime_error("recording did not start");
    recording.Event("console", "Error", "quoted \"message\"\nnext line");
    recording.Frame(120.f, 8.5);
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
