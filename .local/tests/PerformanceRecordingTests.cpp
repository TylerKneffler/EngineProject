#include "Core/View/Views/PerformanceRecording.h"
#include "Diagnostics/PerformanceProvenance.h"
#include "Diagnostics/FrameActivity.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
int main()
{
    using Engine::Editor::FrameActivity;
    FrameActivity::Consume();
    FrameActivity::Mark(FrameActivity::PlayStart);
    FrameActivity::Mark(FrameActivity::Load);
    FrameActivity::Mark(FrameActivity::PlayStop);
    if (FrameActivity::Consume() != (FrameActivity::PlayStart | FrameActivity::Load | FrameActivity::PlayStop) ||
        FrameActivity::Consume() != 0)
        { std::cerr << "Frame activity tags were lost or leaked to the next update\n"; return 5; }
    if (Engine::Editor::PerformanceJsonString("C:\\a\"b\n") != "\"C:\\\\a\\\"b\\u000a\"")
        return 3;
    const auto path=std::filesystem::temp_directory_path()/
        ("engine-metrics-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".jsonl");
    Engine::Editor::PerformanceRecording recording;
    if(!recording.Start(path)) return 1;
    recording.Frame(999.f,20.,1.,25.);
    recording.Frame(999.f,50.,1.);
    recording.Stop();
    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)),{});
    input.close();
    std::filesystem::remove(path);
    if (text.find("{\"type\":\"session\"") != 0 ||
        text.find("\"build_configuration\":\"unknown\"") != std::string::npos ||
        text.find("PerformanceRecordingTests.exe") == std::string::npos)
        { std::cerr << "Missing process identity or session ordering\n"; return 4; }
    for(const char* field:{"\"fps\":50.000", "\"fps_scope\":\"editor_update_interval\"",
        "\"type\":\"session\"", "\"executable_path\":", "\"build_configuration\":", "\"assertions_enabled\":",
        "\"editor_updates_per_second\":50.000", "\"submitted_frames_per_second\":25.000",
        "\"submitted_frames_per_second\":null"})
        if(text.find(field)==std::string::npos) { std::cerr<<"Missing "<<field<<"\n"; return 2; }
    std::cout<<"Performance recording measurement checks passed\n";
}
