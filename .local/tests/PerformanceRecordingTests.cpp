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
#include "Core/Graphics/NativeSubmissionTimings.h"
#include <future>
#include <thread>

void TestAsyncWriter()
{
    const auto path=std::filesystem::current_path()/"engine-async-writer-test.jsonl";
    Engine::Threading::AsyncTextWriter writer(4096,2);
    if(!writer.Start(path)) throw std::runtime_error("async writer did not open");
    std::promise<void> entered,release; auto gate=release.get_future().share();
    const auto owner=std::this_thread::get_id();
    writer.Enqueue([&entered,gate,owner] {
        if(std::this_thread::get_id()==owner) throw std::runtime_error("formatter ran on producer");
        entered.set_value(); gate.wait(); return "{\"sequence\":0}";
    },128);
    if(entered.get_future().wait_for(std::chrono::seconds(5))!=std::future_status::ready)
    { release.set_value(); writer.Stop(); throw std::runtime_error("writer worker did not start"); }
    writer.Enqueue([] { return "{\"sequence\":1}"; },128);
    writer.Enqueue([] { return "{\"sequence\":2}"; },128);
    if(writer.Enqueue([] { return "overflow"; },128))
    { release.set_value(); writer.Stop(); throw std::runtime_error("writer queue is not bounded"); }
    release.set_value(); writer.Stop();
    auto statistics=writer.ReadStatistics();
    if(statistics.written!=3 || statistics.dropped!=1 || statistics.queuedBytes!=0 || statistics.failed)
        throw std::runtime_error("writer did not drain or account for overflow");
    std::ifstream input(path); const std::string contents((std::istreambuf_iterator<char>(input)),{}); input.close();
    if(contents.find("\"sequence\":0")>contents.find("\"sequence\":1") ||
        contents.find("\"sequence\":1")>contents.find("\"sequence\":2") ||
        contents.find("\"dropped_records\":1")==std::string::npos)
        throw std::runtime_error("writer reordered records or omitted loss summary");
    if(!writer.Start(path)) throw std::runtime_error("writer did not restart");
    Engine::Threading::CapturedText captured;
    std::string source="immutable"; captured<<"{\"text\":\""<<source<<"\",\"value\":"<<uint64_t(9007199254740993ull)<<"}";
    source="edited";
    const auto bytes=captured.OwnedBytes();
    writer.Enqueue([captured=std::move(captured)] { return captured.Format(); },bytes);
    // A malformed record must not terminate the writer or prevent draining.
    writer.Enqueue([]()->std::string { throw std::runtime_error("format failure"); },64);
    writer.Stop();
    statistics=writer.ReadStatistics();
    // Use a larger capacity for captured packets: the first packet above can
    // intentionally exceed this tiny queue and must count as an overflow.
    if(statistics.failed!=1 || statistics.dropped!=1) throw std::runtime_error("writer failure/oversize accounting failed");
    Engine::Threading::AsyncTextWriter copied;
    if(!copied.Start(path)) throw std::runtime_error("snapshot writer did not start");
    Engine::Threading::CapturedText snapshot;
    snapshot<<"{\"text\":\""<<std::string("immutable")<<"\",\"value\":"<<uint64_t(9007199254740993ull)<<"}";
    const auto snapshotBytes=snapshot.OwnedBytes();
    copied.Enqueue([snapshot=std::move(snapshot)] { return snapshot.Format(); },snapshotBytes); copied.Stop();
    input.open(path); const std::string copy((std::istreambuf_iterator<char>(input)),{}); input.close();
    if(copy.find("immutable")==std::string::npos || copy.find("9007199254740993")==std::string::npos)
        throw std::runtime_error("captured values lost ownership or integer precision");
    std::filesystem::remove(path);
}

int main()
{
    Engine::Graphics::NativeSubmissionTimings native;
    native.supported=true; native.enabled=true; native.frameSerial=42;
    native.computeArenaWrites=8;
    native.phases[size_t(Engine::Graphics::NativePhase::Shadow)][size_t(Engine::Graphics::NativeOperation::Map)]={3,9007199254740993ull,3,2.5,2};
    Engine::Threading::CapturedText nativeCapture; native.WriteJson(nativeCapture);
    const auto nativeText=nativeCapture.Format();
    for(const char* field : {"\"native_timing_enabled\":true", "\"native_compute_arena_writes\":8",
        "\"native_shadow_map_calls\":3", "\"native_shadow_map_thread_cycles\":9007199254740993"})
        if(nativeText.find(field)==std::string::npos) throw std::runtime_error("native submission fields lost during background formatting");

    TestAsyncWriter();
    Engine::Editor::FrameRateWindow frameRate;
    for(int frame=0;frame<200;++frame) frameRate.Add(10);
    if(frameRate.Fps()!=100) throw std::runtime_error("Frame rate must count frame intervals");
    frameRate.Add(5000);
    if(frameRate.Fps()!=.2) throw std::runtime_error("Frame rate must include idle waits");
    for(int frame=0;frame<100;++frame) frameRate.Add(10);
    frameRate.Add(-1); frameRate.Add(std::numeric_limits<double>::quiet_NaN());
    if(frameRate.Fps()!=100) throw std::runtime_error("Frame rate must recover after idle and ignore invalid intervals");
    const auto stats=Engine::Editor::SummarizeFrames({10,20,30,40,0,-1,std::numeric_limits<double>::quiet_NaN()});
    if(stats.meanMs!=25 || stats.fps!=40 || stats.p95Ms!=40 || stats.p99Ms!=40 || stats.over60BudgetPercent!=75)
        throw std::runtime_error("Frame statistics must use mean intervals and ignore invalid samples");
    Engine::Rendering::RenderCpuTimings view, all;
    view.totalMs=3; view.views=1; view.physicalDraws=4;
    view.workerWaitMs=.25; view.workerTasks=3; view.gpuGrassBatches=2; view.grassPlacementUploadBytes=48;
    view.instances.newBytes=64; view.instances.residentBytes=4096; view.instances.cacheMembership=1;
    all+=view; all+=view;
    if(all.instances.newBytes!=128 || all.instances.residentBytes!=4096 || all.instances.cacheMembership!=2)
        throw std::runtime_error("Instance storage counters or residency gauges aggregated incorrectly");
    if(all.totalMs!=6 || all.views!=2 || all.physicalDraws!=8 || all.workerWaitMs!=.5 || all.workerTasks!=6 || all.gpuGrassBatches!=4 || all.grassPlacementUploadBytes!=96)
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
