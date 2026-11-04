#include "Core/View/Views/PerformanceRecording.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
int main()
{
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
    for(const char* field:{"\"fps\":50.000", "\"fps_scope\":\"editor_update_interval\"",
        "\"editor_updates_per_second\":50.000", "\"submitted_frames_per_second\":25.000",
        "\"submitted_frames_per_second\":null"})
        if(text.find(field)==std::string::npos) { std::cerr<<"Missing "<<field<<"\n"; return 2; }
    std::cout<<"Performance recording measurement checks passed\n";
}
