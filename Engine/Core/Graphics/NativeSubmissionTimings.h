#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Engine::Graphics
{
enum class NativePhase : uint32_t { Other,Camera,Shadow,Count };
enum class NativeOperation : uint32_t { Map,Upload,Bind,Dispatch,IndirectArguments,Draw,ResourceCreate,Readback,Count };
struct NativeCallTiming
{
    uint64_t calls=0,threadCycles=0,cycleSamples=0;
    double milliseconds=0,maximumMs=0;
};
struct NativeSubmissionTimings
{
    std::array<std::array<NativeCallTiming,size_t(NativeOperation::Count)>,size_t(NativePhase::Count)> phases{};
    uint64_t frameSerial=0;
    uint64_t computeArenaWrites=0,computeDiscardMaps=0;
    bool supported=false,enabled=false;
    template<class Stream> void WriteJson(Stream& output) const
    {
        output<<",\"native_timing_supported\":"<<(supported ? "true":"false")
            <<",\"native_timing_enabled\":"<<(enabled ? "true":"false")<<",\"native_frame_serial\":"<<frameSerial
            <<",\"native_compute_arena_writes\":"<<computeArenaWrites<<",\"native_compute_discard_maps\":"<<computeDiscardMaps;
        if(!enabled) return;
        static const char* phaseNames[]={"other","camera","shadow"};
        static const char* operationNames[]={"map","upload","bind","dispatch","indirect_args","draw","resource_create","readback"};
        for(size_t phase=0;phase<phases.size();++phase) for(size_t op=0;op<phases[phase].size();++op)
        {
            const auto& value=phases[phase][op];
            const auto key=[&] { output<<",\"native_"<<phaseNames[phase]<<'_'<<operationNames[op]; };
            key(); output<<"_calls\":"<<value.calls;
            key(); output<<"_ms\":"<<value.milliseconds;
            key(); output<<"_max_ms\":"<<value.maximumMs;
            key(); output<<"_thread_cycles\":"<<value.threadCycles;
            key(); output<<"_cycle_samples\":"<<value.cycleSamples;
        }
    }
};
// Owned by one graphics device's submission thread, shared by its context and
// buffer factories. Explicit APIs can supply their own thread-local collectors.
struct NativeSubmissionProbe
{
    NativeSubmissionTimings timings;
    NativePhase phase=NativePhase::Other;
    void Reset(uint64_t serial)
    { const bool enabled=timings.enabled; timings={}; timings.supported=true; timings.enabled=enabled; timings.frameSerial=serial; phase=NativePhase::Other; }
};
class NativeCallScope
{
    using Clock=std::chrono::steady_clock;
    NativeCallTiming* m_target=nullptr;
    Clock::time_point m_started;
    uint64_t m_cycles=0;
    bool m_haveCycles=false;
public:
    NativeCallScope(NativeSubmissionProbe* probe,NativeOperation operation)
    {
        if(!probe || !probe->timings.enabled) return;
        m_target=&probe->timings.phases[size_t(probe->phase)][size_t(operation)];
#if defined(_WIN32)
        ULONG64 cycles=0; m_haveCycles=QueryThreadCycleTime(GetCurrentThread(),&cycles)!=FALSE; m_cycles=cycles;
#endif
        m_started=Clock::now();
    }
    ~NativeCallScope()
    {
        if(!m_target) return;
        const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-m_started).count();
#if defined(_WIN32)
        ULONG64 cycles=0;
        if(m_haveCycles && QueryThreadCycleTime(GetCurrentThread(),&cycles) && cycles>=m_cycles)
        { m_target->threadCycles+=cycles-m_cycles; ++m_target->cycleSamples; }
#endif
        ++m_target->calls; m_target->milliseconds+=elapsed;
        if(elapsed>m_target->maximumMs) m_target->maximumMs=elapsed;
    }
};
template<class F> decltype(auto) MeasureNative(NativeSubmissionProbe* probe,NativeOperation operation,F&& function)
{
    NativeCallScope timing(probe,operation);
    return std::forward<F>(function)();
}
}
