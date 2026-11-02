#include "PerformanceView.h"
#include "PerformanceRecording.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Engine/Editor/Diagnostics/EditorFrameProfiler.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

namespace Engine::Editor
{
namespace
{
constexpr EditorUiColor kUserColor{ .20f, .70f, 1.f, 1.f };
constexpr EditorUiColor kKernelColor{ .65f, .38f, 1.f, 1.f };
constexpr EditorUiColor kProcessColor{ .18f, .82f, .57f, 1.f };
constexpr EditorUiColor kCacheColor{ .95f, .68f, .20f, 1.f };
constexpr EditorUiColor kOtherMemoryColor{ .25f, .53f, .75f, 1.f };
constexpr std::array<EditorUiColor, 6> kGpuColors{{
    { .30f, .82f, .42f, 1.f }, { .10f, .67f, .75f, 1.f },
    { .95f, .58f, .18f, 1.f }, { .90f, .34f, .50f, 1.f },
    { .62f, .45f, .92f, 1.f }, { .65f, .65f, .65f, 1.f }
}};
constexpr const char* kGpuLabels[] = {
    "3D", "Compute", "Copy", "Video decode", "Video encode", "Other"
};

std::string FormatBytes(uint64_t bytes)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(unit > 2 ? 2 : 1)
        << value << ' ' << units[unit];
    return stream.str();
}

float Percent(uint64_t value, uint64_t total)
{
    return total ? std::clamp(100.f * static_cast<float>(
        static_cast<double>(value) / static_cast<double>(total)), 0.f, 100.f) : 0.f;
}

std::string Narrow(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1,
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(length - 1, 0)), '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, value, -1,
            result.data(), length - 1, nullptr, nullptr);
    return result;
}

template<typename T>
void PushHistory(std::deque<T>& history, T value)
{
    constexpr size_t kHistorySamples = 120;
    history.push_back(value);
    if (history.size() > kHistorySamples) history.pop_front();
}
}

class PerformanceView::Sampler
{
public:
    explicit Sampler(DWORD mainThreadId)
    {
        m_mainThread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,mainThreadId);
        QueryHardware();
        QueryProcessorTimes(m_previousProcessors);
        InitializeGpuCounters();
    }

    ~Sampler()
    {
        if(m_mainThread) CloseHandle(m_mainThread);
        if (m_gpuQuery) PdhCloseQuery(m_gpuQuery);
    }

    const PerformanceHardware& Hardware() const { return m_hardware; }

    PerformanceSnapshot Sample()
    {
        PerformanceSnapshot result;
        SampleProcessors(result);
        SampleProcessCpu(result);
        SampleMemory(result);
        SampleGpu(result);
        return result;
    }

private:
    struct ProcessorTimes
    {
        LARGE_INTEGER idle{};
        LARGE_INTEGER kernel{};
        LARGE_INTEGER user{};
        LARGE_INTEGER dpc{};
        LARGE_INTEGER interrupt{};
        ULONG interruptCount = 0;
    };
    using NtQuerySystemInformationFn = LONG (WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    struct GpuCounter
    {
        PDH_HCOUNTER handle = nullptr;
        int category = 0;
        std::string engineId;
        std::wstring path;
    };

    bool QueryProcessorTimes(std::vector<ProcessorTimes>& values)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto query = ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation")) : nullptr;
        if (!query) return false;
        values.resize(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        ULONG returned = 0;
        const LONG status = query(8, values.data(),
            static_cast<ULONG>(values.size() * sizeof(ProcessorTimes)), &returned);
        if (status < 0) { values.clear(); return false; }
        values.resize(returned / sizeof(ProcessorTimes));
        return true;
    }

    void SampleProcessors(PerformanceSnapshot& result)
    {
        std::vector<ProcessorTimes> current;
        if (!QueryProcessorTimes(current)) return;
        result.cores.resize(current.size());
        uint64_t allUser = 0, allKernel = 0, allTotal = 0;
        if (m_previousProcessors.size() == current.size())
        {
            for (size_t index = 0; index < current.size(); ++index)
            {
                const uint64_t idle = current[index].idle.QuadPart -
                    m_previousProcessors[index].idle.QuadPart;
                const uint64_t kernel = current[index].kernel.QuadPart -
                    m_previousProcessors[index].kernel.QuadPart;
                const uint64_t user = current[index].user.QuadPart -
                    m_previousProcessors[index].user.QuadPart;
                const uint64_t total = kernel + user;
                const uint64_t busyKernel = kernel > idle ? kernel - idle : 0;
                result.cores[index].user = Percent(user, total);
                result.cores[index].kernel = Percent(busyKernel, total);
                allUser += user;
                allKernel += busyKernel;
                allTotal += total;
            }
            result.cpuUser = Percent(allUser, allTotal);
            result.cpuKernel = Percent(allKernel, allTotal);
        }
        m_previousProcessors = std::move(current);
    }

    static uint64_t Ticks(FILETIME time)
    { return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime; }
    void SampleProcessCpu(PerformanceSnapshot& result)
    {
        FILETIME creation{},exit{},kernel{},user{};
        const auto now=std::chrono::steady_clock::now();
        if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)) return;
        const auto process=Ticks(kernel)+Ticks(user);
        uint64_t thread=0;
        if(m_mainThread && GetThreadTimes(m_mainThread,&creation,&exit,&kernel,&user)) thread=Ticks(kernel)+Ticks(user);
        if(m_previousProcessTime.time_since_epoch().count())
        {
            const double elapsed=std::chrono::duration<double>(now-m_previousProcessTime).count()*1.e7;
            if(elapsed>0)
            {
                result.processCpuCores=float(double(process-m_previousProcess)/elapsed);
                result.processCpuPercent=std::clamp(100.f*result.processCpuCores/std::max(1u,m_hardware.logicalCores),0.f,100.f);
                if(thread>=m_previousThread) result.mainThreadCpuPercent=std::clamp(float(100.*double(thread-m_previousThread)/elapsed),0.f,100.f);
            }
        }
        m_previousProcess=process; m_previousThread=thread; m_previousProcessTime=now;
    }
    HANDLE m_mainThread=nullptr;
    uint64_t m_previousProcess=0,m_previousThread=0;
    std::chrono::steady_clock::time_point m_previousProcessTime{};

    void SampleMemory(PerformanceSnapshot& result)
    {
        MEMORYSTATUSEX memory{ sizeof(memory) };
        if (GlobalMemoryStatusEx(&memory))
        {
            result.memoryTotal = memory.ullTotalPhys;
            result.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        }
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
            result.processMemory = process.WorkingSetSize;

        PERFORMANCE_INFORMATION performance{ sizeof(performance) };
        if (GetPerformanceInfo(&performance, sizeof(performance)))
        {
            result.systemCache = static_cast<uint64_t>(performance.SystemCache) *
                performance.PageSize;
            result.commitUsed = static_cast<uint64_t>(performance.CommitTotal) *
                performance.PageSize;
            result.commitLimit = static_cast<uint64_t>(performance.CommitLimit) *
                performance.PageSize;
        }
    }

    void InitializeGpuCounters()
    {
        if (PdhOpenQueryW(nullptr, 0, &m_gpuQuery) != ERROR_SUCCESS) return;
        RefreshGpuCounters();
        if (!m_gpuCounters.empty()) PdhCollectQueryData(m_gpuQuery);
    }

    void RefreshGpuCounters()
    {
        if (!m_gpuQuery) return;
        DWORD length = 0;
        PdhExpandWildCardPathW(nullptr,
            L"\\GPU Engine(*)\\Utilization Percentage", nullptr, &length,
            PDH_NOEXPANDCOUNTERS);
        if (!length) return;
        std::vector<wchar_t> paths(length);
        if (PdhExpandWildCardPathW(nullptr,
            L"\\GPU Engine(*)\\Utilization Percentage", paths.data(), &length,
            PDH_NOEXPANDCOUNTERS) != ERROR_SUCCESS) return;
        std::set<std::wstring> live;
        for (const wchar_t* path = paths.data(); *path; path += wcslen(path) + 1)
        {
            live.emplace(path);
            if (std::any_of(m_gpuCounters.begin(), m_gpuCounters.end(),
                [path](const GpuCounter& counter) { return counter.path == path; }))
                continue;
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddEnglishCounterW(m_gpuQuery, path, 0, &counter) != ERROR_SUCCESS)
                continue;
            std::wstring lower(path);
            std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            int category = 5;
            if (lower.find(L"engtype_3d") != std::wstring::npos) category = 0;
            else if (lower.find(L"engtype_compute") != std::wstring::npos) category = 1;
            else if (lower.find(L"engtype_copy") != std::wstring::npos) category = 2;
            else if (lower.find(L"engtype_video decode") != std::wstring::npos) category = 3;
            else if (lower.find(L"engtype_video encode") != std::wstring::npos) category = 4;
            // GPU Engine exposes one counter per process and engine. Aggregate
            // processes by adapter LUID, physical adapter and engine index.
            const auto luid = lower.find(L"luid_");
            const auto type = lower.find(L"_engtype_", luid);
            if (luid == std::wstring::npos || type == std::wstring::npos)
            {
                PdhRemoveCounter(counter);
                continue;
            }
            const std::wstring identity = lower.substr(luid, type - luid);
            m_gpuCounters.push_back({ counter, category, Narrow(identity.c_str()), path });
        }
        m_gpuCounters.erase(std::remove_if(m_gpuCounters.begin(), m_gpuCounters.end(),
            [&live](const GpuCounter& counter)
            {
                if (live.find(counter.path) != live.end()) return false;
                PdhRemoveCounter(counter.handle);
                return true;
            }), m_gpuCounters.end());
    }

    void SampleGpu(PerformanceSnapshot& result)
    {
        if (++m_gpuRefreshSamples >= 10)
        {
            m_gpuRefreshSamples = 0;
            RefreshGpuCounters();
        }
        std::map<std::pair<std::string, int>, float> engines;
        if (m_gpuQuery && PdhCollectQueryData(m_gpuQuery) == ERROR_SUCCESS)
        {
            for (const GpuCounter& counter : m_gpuCounters)
            {
                PDH_FMT_COUNTERVALUE value{};
                if (PdhGetFormattedCounterValue(counter.handle, PDH_FMT_DOUBLE,
                    nullptr, &value) == ERROR_SUCCESS &&
                    value.CStatus == PDH_CSTATUS_VALID_DATA && std::isfinite(value.doubleValue))
                    engines[{counter.engineId, counter.category}] +=
                        static_cast<float>(std::max(0.0, value.doubleValue));
            }
            for (const auto& [key, value] : engines)
            {
                const float usage = std::clamp(value, 0.f, 100.f);
                result.gpuEngineInstances.push_back({key.first,
                    static_cast<uint32_t>(key.second), usage});
                result.gpuEngines[key.second] = std::max(result.gpuEngines[key.second], usage);
            }
        }
        if (m_adapter)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            if (SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
            {
                result.gpuMemoryUsed = memory.CurrentUsage;
                result.gpuMemoryBudget = memory.Budget;
            }
        }
    }

    void QueryHardware()
    {
        m_hardware.logicalCores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
            KEY_READ, &key) == ERROR_SUCCESS)
        {
            wchar_t name[256]{};
            DWORD nameBytes = sizeof(name);
            if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                reinterpret_cast<BYTE*>(name), &nameBytes) == ERROR_SUCCESS)
                m_hardware.cpuName = Narrow(name);
            DWORD mhz = 0, mhzBytes = sizeof(mhz);
            if (RegQueryValueExW(key, L"~MHz", nullptr, nullptr,
                reinterpret_cast<BYTE*>(&mhz), &mhzBytes) == ERROR_SUCCESS)
                m_hardware.cpuMHz = mhz;
            RegCloseKey(key);
        }

        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
        std::vector<unsigned char> topology(bytes);
        if (bytes && GetLogicalProcessorInformationEx(RelationAll,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(topology.data()),
            &bytes))
        {
            for (DWORD offset = 0; offset < bytes; )
            {
                const auto* item = reinterpret_cast<const
                    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(topology.data() + offset);
                if (item->Relationship == RelationProcessorCore)
                    ++m_hardware.physicalCores;
                else if (item->Relationship == RelationCache &&
                    item->Cache.Level >= 1 && item->Cache.Level <= 4)
                    m_hardware.cacheBytes[item->Cache.Level - 1] += item->Cache.CacheSize;
                if (!item->Size) break;
                offset += item->Size;
            }
        }
        ULONGLONG installedKb = 0;
        if (GetPhysicallyInstalledSystemMemory(&installedKb))
            m_hardware.installedMemory = installedKb * 1024ull;

        ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
        for (UINT index = 0; ; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description{};
            adapter->GetDesc1(&description);
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            m_hardware.gpuName = Narrow(description.Description);
            m_hardware.dedicatedVideoMemory = description.DedicatedVideoMemory;
            m_hardware.sharedVideoMemory = description.SharedSystemMemory;
            adapter.As(&m_adapter);
            break;
        }
    }

    PerformanceHardware m_hardware;
    std::vector<ProcessorTimes> m_previousProcessors;
    PDH_HQUERY m_gpuQuery = nullptr;
    std::vector<GpuCounter> m_gpuCounters;
    unsigned m_gpuRefreshSamples = 0;
    ComPtr<IDXGIAdapter3> m_adapter;
};

PerformanceView::PerformanceView(Engine::Scene::Scene* scene,
    std::shared_ptr<PerformanceRecording> recording)
    : m_scene(scene), m_recording(std::move(recording))
{
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
    const DWORD mainThreadId=GetCurrentThreadId();
    m_samplerWorker = std::thread([this,mainThreadId] {
        Sampler sampler(mainThreadId);
        {
            std::lock_guard lock(m_samplerMutex);
            m_pendingHardware = sampler.Hardware();
        }
        for (;;)
        {
            std::unique_lock lock(m_samplerMutex);
            m_samplerWake.wait(lock, [this] {
                return m_stopSampler || m_samplerActive;
            });
            if (m_stopSampler) break;
            lock.unlock();
            const auto started = std::chrono::steady_clock::now();
            auto snapshot = sampler.Sample();
            const double sampleMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            lock.lock();
            if (m_stopSampler) break;
            m_pendingSnapshot = std::move(snapshot);
            m_pendingCounterSampleMs = sampleMs;
            ++m_samplerVersion;
            if (m_samplerWake.wait_for(lock, std::chrono::milliseconds(500),
                [this] { return m_stopSampler || !m_samplerActive; }) &&
                m_stopSampler) break;
        }
    });
}

PerformanceView::~PerformanceView()
{
    StopRecording();
    {
        std::lock_guard lock(m_samplerMutex);
        m_stopSampler = true;
    }
    m_samplerWake.notify_one();
    if (m_samplerWorker.joinable()) m_samplerWorker.join();
}

bool PerformanceView::IsRecording() const
{
    return m_recording && m_recording->IsRecording();
}

void PerformanceView::SetFrameTimings(const EditorFrameProfileSample& sample,
    Renderers::IEditorRenderer* renderer)
{
    const auto ms = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    m_renderCpu=sample.renderCpu;
    m_stageWork=sample.stages;
    m_frameLatencyMs=sample.deltaSeconds*1000.;
    if(sample.deltaSeconds>0 && std::isfinite(sample.deltaSeconds))
    {
        const double interval=sample.deltaSeconds*1000.;
        m_editorFrameRate.Add(interval);
        m_displayFrameRate=static_cast<float>(m_editorFrameRate.Fps());
        m_frameIntervals.push_back(interval); m_frameIntervalTotal+=interval; m_statisticsElapsed+=interval;
        while(m_frameIntervals.size()>1 && (m_frameIntervalTotal>5000. || m_frameIntervals.size()>600))
        { m_frameIntervalTotal-=m_frameIntervals.front(); m_frameIntervals.pop_front(); }
        if(m_statisticsElapsed>=250. || !m_frameStatistics.meanMs)
        { m_frameStatistics=SummarizeFrames(std::vector<double>(m_frameIntervals.begin(),m_frameIntervals.end())); m_statisticsElapsed=0; }
    }
    if (IsRecording())
    {
        m_recording->Frame(m_lastUiFrameRate,sample.deltaSeconds*1000.,m_lastPanelDrawMs);
        if (!IsOpen()) Refresh(sample.deltaSeconds > 0.f ? 1.f / sample.deltaSeconds : 0.f);
        m_recording->FrameDetails(
            EditorFrameProfiler::CaptureFields(sample, m_scene, renderer));
    }
    m_hasFrameTimings = true;
    m_frameTimings = {
        { "Frame", ms(sample.frameBegin, sample.afterPendingLoad) },
        { "Pacing", ms(sample.frameBegin, sample.afterPacing) },
        { "Background", ms(sample.afterPacing, sample.afterBackground) },
        { "Scene update", ms(sample.afterBackground, sample.afterSceneUpdate) },
        { "Render", ms(sample.afterSceneUpdate, sample.afterRender) },
        { "Pending load", ms(sample.afterRender, sample.afterPendingLoad) }
    };
    m_updateTimings.clear();
    if (m_scene && sample.playing)
    {
        const auto& update = m_scene->GetLastUpdateTimings();
        m_updateTimings = {
            { "Objects", update.objectsMs }, { "Pose / constraints", update.constraintsMs }, { "Audio", update.audioMs },
            { "Physics", update.physicsMs },
            { "Post physics", update.postPhysicsMs }
        };
        if (auto* bullet = dynamic_cast<Physics::BulletPhysicsAdapter*>(
            &m_scene->GetPhysics().GetAdapter()))
        {
            const auto physics = bullet->GetLastStepTimings();
            m_updateTimings.push_back({ "Physics setup", physics.setupMs });
            m_updateTimings.push_back({ "Physics simulation", physics.simulationMs });
            m_updateTimings.push_back({ "Physics post", physics.postMs });
        }
    }
    m_renderTimings = {
        { "Prepare", sample.prepareMilliseconds },
        { "Render entry", sample.renderEntryMilliseconds },
        { "Clear", sample.clearMilliseconds },
        { "Scene preparation", sample.scenePrepareMilliseconds },
        { "Views", sample.viewMilliseconds },
        { "Shadow pass (CPU)", sample.shadowSubmitMilliseconds },
        { "Main pass (CPU)", sample.mainSubmitMilliseconds },
        { "Editor UI", sample.editorUiMilliseconds }
    };
    if (auto* dx11 = dynamic_cast<Renderers::DX11EditorRenderer*>(renderer))
    {
        m_renderTimings.push_back({ "Render setup", dx11->GetLastRenderSetupMilliseconds() });
        m_renderTimings.push_back({ "Begin UI", dx11->GetLastBeginUiMilliseconds() });
        m_renderTimings.push_back({ "UI render", dx11->GetLastUiRenderMilliseconds() });
        m_renderTimings.push_back({ "Present", dx11->GetLastPresentMilliseconds() });
    }
    m_gpuTimings.clear();
    m_gpuQueryTimings.clear();
    m_gpuTimingsValid = false;
    if (renderer && renderer->GetGraphicsProvider())
        if (auto* factory = renderer->GetGraphicsProvider()->GetContextFactory())
        {
            m_nativeTimings=factory->GetNativeSubmissionTimings();
            if(m_nativeTimingChange) { factory->SetNativeCallTimingEnabled(m_nativeTimingRequested); m_nativeTimingChange=false; }
            else m_nativeTimingRequested=m_nativeTimings.enabled;
            const auto timing = factory->GetFrameTimingTelemetry();
            m_gpuQueryTimings = {
                { "Query resolve (CPU)", timing.cpuGpuQueryResolveMilliseconds },
                { "Query begin (CPU)", timing.cpuGpuQueryBeginMilliseconds },
                { "GPU submission (CPU)", timing.cpuSubmissionMilliseconds },
                { "GPU presentation (CPU)", timing.cpuPresentationMilliseconds }
            };
            m_gpuTimingsValid = timing.gpuTimingsValid;
            m_gpuSampleId=timing.gpuSampleId; m_gpuRegions=timing.gpuRegionCount; m_gpuTotalMs=0;
            if(m_gpuTimingsValid) for(double value:timing.gpuMilliseconds) m_gpuTotalMs+=value;
            if (m_gpuTimingsValid)
            {
                using Stage = Graphics::GpuTimingStage;
                m_gpuTimings = {
                    { "Shadow", timing.gpuMilliseconds[static_cast<size_t>(Stage::Shadow)] },
                    { "Terrain", timing.gpuMilliseconds[static_cast<size_t>(Stage::Terrain)] },
                    { "Grass", timing.gpuMilliseconds[static_cast<size_t>(Stage::Grass)] },
                    { "Portal", timing.gpuMilliseconds[static_cast<size_t>(Stage::Portal)] },
                    { "Opaque", timing.gpuMilliseconds[static_cast<size_t>(Stage::Opaque)] },
                    { "Transparent", timing.gpuMilliseconds[static_cast<size_t>(Stage::Transparent)] }
                };
            }
        }
}

void PerformanceView::DrawTimings(IEditorUi& ui)
{
    ui.ValueLabel("Performance panel (previous draw)",
        (std::to_string(m_lastPanelDrawMs) + " ms").c_str());
    ui.ValueLabel("Hardware counters (background)",
        (std::to_string(m_lastCounterSampleMs) + " ms").c_str());
    ui.Separator();
    if (!m_hasFrameTimings)
    {
        ui.DisabledLabel("Waiting for a frame sample.");
        return;
    }
    const auto drawGroup = [&](const char* title,
        const std::vector<TimingValue>& values) {
        ui.Label(title);
        for (const auto& value : values)
        {
            std::ostringstream formatted;
            formatted << std::fixed << std::setprecision(2)
                << value.milliseconds << " ms";
            ui.ValueLabel(value.label, formatted.str().c_str());
        }
        ui.Separator();
    };
    drawGroup("Frame intervals (rolling five seconds)", {
        {"Average interval",m_frameStatistics.meanMs},{"95th percentile",m_frameStatistics.p95Ms},{"99th percentile",m_frameStatistics.p99Ms}
    });
    ui.ValueLabel("Average FPS",std::to_string(m_frameStatistics.fps).c_str());
    ui.ValueLabel("Frames over 60 FPS budget (%)",std::to_string(m_frameStatistics.over60BudgetPercent).c_str());
    ui.DisabledLabel("Frame interval includes waits between updates; CPU frame work can be shorter. The 60 FPS budget is 16.67 ms.");
    drawGroup("Frame phases (CPU)", m_frameTimings);
    ui.Label("Stage work (ms)");
    ui.DisabledLabel("Columns: wall / main busy / worker execution / queue delay / dependency wait / scheduler wait / render stall. Worker and queue values sum across engine jobs; stages can overlap.");
    static constexpr const char* stageNames[]={"Pacing","Background","Scene update","Render","Pending load"};
    for(size_t i=0;i<m_stageWork.size();++i)
    {
        const auto& stage=m_stageWork[i];
        std::ostringstream values;
        values << std::fixed << std::setprecision(2) << stage.wallMs << " / "
            << stage.mainBusyMs << " / " << stage.workerMs << " / "
            << stage.queueMs << " / " << stage.dependencyMs << " / "
            << stage.waitMs << " / " << stage.renderStallMs;
        ui.ValueLabel(stageNames[i],values.str().c_str());
    }
    ui.ValueLabel("Frame latency (update interval)",
        (std::to_string(m_frameLatencyMs)+" ms").c_str());
    ui.DisabledLabel("Render stall estimates time the main thread was not executing CPU instructions during render; it includes waits and preemption.");
    ui.Separator();
    if (!m_updateTimings.empty())
        drawGroup("Scene update (CPU)", m_updateTimings);
    drawGroup("Draw and render (CPU)", m_renderTimings);
    ui.BeginDisabled(!m_nativeTimings.supported);
    if(ui.Checkbox("Native graphics call timing",&m_nativeTimingRequested)) m_nativeTimingChange=true;
    ui.EndDisabled();
    if(m_nativeTimings.supported)
    {
        ui.ValueLabel("Compute constant arena writes",std::to_string(m_nativeTimings.computeArenaWrites).c_str());
        ui.ValueLabel("Compute constant fallback discards",std::to_string(m_nativeTimings.computeDiscardMaps).c_str());
    }
    if(m_nativeTimings.enabled)
    {
        static const char* names[]={"Map","Unmap","Upload","Bindings","Dispatch","Indirect argument copy","Draw","Resource creation","Readback","Fence wait"};
        static_assert(std::size(names)==size_t(Graphics::NativeOperation::Count));
        for(size_t phase=0;phase<m_nativeTimings.phases.size();++phase)
        {
            std::vector<TimingValue> values;
            for(size_t operation=0;operation<m_nativeTimings.phases[phase].size();++operation)
                values.push_back({names[operation],m_nativeTimings.phases[phase][operation].milliseconds});
            drawGroup(phase==0 ? "Native calls outside passes (CPU)":phase==1 ? "Native camera calls (CPU)":"Native shadow calls (CPU)",values);
        }
        ui.DisabledLabel("Diagnostic timing adds overhead. Recordings include call maxima and thread cycles; low cycles alone do not prove a GPU wait.");
    }
    if (m_scene)
    {
        const auto& prepare = m_scene->GetLastPrepareTimings();
        drawGroup("Scene preparation (CPU)", {
            { "Transforms", prepare.transformsMs }, { "Lookup", prepare.lookupMs }, { "Items", prepare.itemsMs },
            { "Bone upload", prepare.boneUploadMs },
            { "Capacity", prepare.capacityMs }, { "Spatial hierarchy", prepare.spatialMs }
        });
        drawGroup("Transform preparation detail (CPU wall time)", {
            { "Capture",prepare.transformSnapshotMs }, { "Compute and joins",prepare.transformComputeMs },
            { "Commit",prepare.transformCommitMs }
        });
        drawGroup("Spatial index detail (CPU wall time)", {
            { "Compare",prepare.spatialCompareMs }, { "Refit and joins",prepare.spatialRefitMs },
            { "Rebuild",prepare.spatialBuildMs }
        });
        const auto& grass = m_scene->GetGrassPerformance();
        drawGroup("Grass (CPU)", {
            { "Total", grass.totalMs }, { "Placement indexing", grass.scanMs },
            { "Instance preparation", grass.generationMs }, { "GPU upload", grass.uploadMs }
        });
        ui.ValueLabel("Grass pending tiles", std::to_string(grass.pending).c_str());
        ui.ValueLabel("Grass LOD switches", std::to_string(grass.lodSwitches).c_str());
        ui.ValueLabel("Grass instance tiles prepared", std::to_string(grass.tilesBuilt).c_str());
        ui.ValueLabel("Grass placements scanned", std::to_string(grass.placementsScanned).c_str());
        ui.ValueLabel("Grass resident LODs", std::to_string(grass.residentLods).c_str());
        ui.ValueLabel("Grass GPU memory", FormatBytes(grass.gpuBytes).c_str());
        ui.Separator();
        ui.ValueLabel("Grass placement instances",std::to_string(grass.instances).c_str());
        ui.ValueLabel("Grass placement uploads",FormatBytes(grass.instanceUploadBytes).c_str());
        ui.ValueLabel("Grass active / LOD transition / fading tiles",
            (std::to_string(grass.activeTiles)+" / "+std::to_string(grass.transitionTiles)+" / "+std::to_string(grass.fadingTiles)).c_str());
        ui.Separator();
        const auto& shadow = m_scene->GetLastShadowCpuTiming();
        ui.ValueLabel("Shadow draws: explicit instances / automatic / ordinary",
            (std::to_string(shadow.explicitInstanceDraws)+" / "+std::to_string(shadow.automaticDraws)+" / "+std::to_string(shadow.ordinaryDraws)).c_str());
        ui.ValueLabel("Shadow submission thread cycles", std::to_string(shadow.submissionThreadCycles).c_str());
        ui.ValueLabel("Shadow instance new / edited / growth bytes",(FormatBytes(shadow.instances.newBytes)+" / "+FormatBytes(shadow.instances.editedBytes)+" / "+FormatBytes(shadow.instances.growthBytes)).c_str());
        ui.ValueLabel("Shadow instance metadata / allocations",(FormatBytes(shadow.instances.metadataBytes)+" / "+std::to_string(shadow.instances.allocations)).c_str());
        drawGroup("Shadows (CPU)", {
            { "Total", shadow.totalMs },
            { "Scene preparation", shadow.scenePreparationMs },
            { "Cascade culling", shadow.cascadeCullingMs },
            { "Record upload", shadow.recordUploadMs },
            { "Draw submission", shadow.drawSubmissionMs },
            { "Constant binding", shadow.constantBindingMs },
            { "Texture binding", shadow.textureBindingMs },
            { "Geometry draws", shadow.geometryDrawMs },
            { "Submission wall time", shadow.submissionWallMs },
            { "Slowest batch", shadow.maxBatchMs },
            { "Pass restore", shadow.passRestoreMs },
            { "Portals", shadow.portalMs }
        });
    }
    ui.ValueLabel("GPU grass batches / reused batches",(std::to_string(m_renderCpu.gpuGrassBatches)+" / "+std::to_string(m_renderCpu.grassBatchCacheHits)).c_str());
    ui.ValueLabel("Grass batch placement uploads",FormatBytes(m_renderCpu.grassPlacementUploadBytes).c_str());
    ui.ValueLabel("Instance pool resident",FormatBytes(m_renderCpu.instances.residentBytes).c_str());
    ui.ValueLabel("Instance new / edited / growth bytes",(FormatBytes(m_renderCpu.instances.newBytes)+" / "+FormatBytes(m_renderCpu.instances.editedBytes)+" / "+FormatBytes(m_renderCpu.instances.growthBytes)).c_str());
    ui.ValueLabel("Instance metadata / visible IDs",(FormatBytes(m_renderCpu.instances.metadataBytes)+" / "+FormatBytes(m_renderCpu.instances.visibleIdBytes)).c_str());
    ui.ValueLabel("Custom shader instance fallback uploads",FormatBytes(m_renderCpu.instances.fallbackBytes).c_str());
    ui.ValueLabel("Instance allocations / retirements",(std::to_string(m_renderCpu.instances.allocations)+" / "+std::to_string(m_renderCpu.instances.retirements)).c_str());
    ui.ValueLabel("Instance misses: cold / membership / metadata / mode",(std::to_string(m_renderCpu.instances.cacheCold)+" / "+std::to_string(m_renderCpu.instances.cacheMembership)+" / "+std::to_string(m_renderCpu.instances.cacheMetadata)+" / "+std::to_string(m_renderCpu.instances.cacheMode)).c_str());
    ui.ValueLabel("Worker jobs / tasks",(std::to_string(m_renderCpu.workerJobs)+" / "+std::to_string(m_renderCpu.workerTasks)).c_str());
    ui.ValueLabel("Configured workers",std::to_string(m_renderCpu.configuredWorkers).c_str());
    if(m_scene)
    {
        const auto& prepare=m_scene->GetLastPrepareTimings();
        ui.ValueLabel("Transform world reused / updated",(std::to_string(prepare.transformWorldReused)+" / "+std::to_string(prepare.transformWorldUpdated)).c_str());
        ui.ValueLabel("Transform locals updated",std::to_string(prepare.transformLocalUpdated).c_str());
        ui.ValueLabel("Transform hierarchy levels",std::to_string(prepare.transformLevels).c_str());
        ui.ValueLabel("Transform structure-change fallback",std::to_string(prepare.transformStructureFallback).c_str());
        ui.ValueLabel("Spatial index changed / reused bounds",(std::to_string(prepare.spatialChangedBounds)+" / "+std::to_string(prepare.spatialReusedBounds)).c_str());
        ui.ValueLabel("Spatial dirty leaves / refit nodes",(std::to_string(prepare.spatialDirtyLeaves)+" / "+std::to_string(prepare.spatialRefitNodes)).c_str());
        ui.ValueLabel("Spatial rebuilds / quality rebuilds",(std::to_string(prepare.spatialRebuilds)+" / "+std::to_string(prepare.spatialQualityRebuilds)).c_str());
        ui.ValueLabel("Bounds reused / updated",(std::to_string(prepare.boundsReused)+" / "+std::to_string(prepare.boundsUpdated)).c_str());
        ui.ValueLabel("Preparation worker / wait ms",(std::to_string(prepare.workerMs)+" / "+std::to_string(prepare.workerWaitMs)).c_str());
    }
    drawGroup("Main render passes (all views, CPU)", {
        {"Measured total",m_renderCpu.totalMs},{"Camera / lighting / setup",m_renderCpu.startupMs},
        {"Terrain occluder preparation",m_renderCpu.terrainPreparationMs},{"View culling",m_renderCpu.cullingMs},
        {"Sorting",m_renderCpu.sortingMs},{"Occlusion queries / signature",m_renderCpu.occlusionMs},
        {"Material preparation",m_renderCpu.materialMs},{"Batch preparation",m_renderCpu.batchingMs},
        {"Data uploads",m_renderCpu.uploadMs},{"Draw submission",m_renderCpu.submissionMs},
        {"Retained UI",m_renderCpu.retainedUiMs},{"Portals / overlays",m_renderCpu.portalMs},
        {"Worker execution (summed)",m_renderCpu.workerActiveMs},{"Main-thread jobs",m_renderCpu.mainJobMs},
        {"Worker synchronization",m_renderCpu.workerWaitMs},{"Record packing (workers)",m_renderCpu.recordPackingMs},
        {"Worker queue delay (summed)",m_renderCpu.workerQueueMs},{"Dependency readiness (summed)",m_renderCpu.workerDependencyMs},
        {"Record synchronization",m_renderCpu.recordWaitMs},{"Other",m_renderCpu.otherMs}
    });
    ui.ValueLabel("Draw candidates / physical draws",(std::to_string(m_renderCpu.candidateDraws)+" / "+std::to_string(m_renderCpu.physicalDraws)).c_str());
    ui.ValueLabel("Automatic batches / GPU batches",(std::to_string(m_renderCpu.automaticBatches)+" / "+std::to_string(m_renderCpu.gpuBatches)).c_str());
    ui.ValueLabel("Automatic instance candidates",std::to_string(m_renderCpu.automaticInstances).c_str());
    ui.ValueLabel("Triangle candidates",std::to_string(m_renderCpu.candidateTriangles).c_str());
    ui.ValueLabel("Changed rendering uploads",FormatBytes(m_renderCpu.changedUploadBytes).c_str());
    ui.DisabledLabel("Instance and triangle candidates are counted before GPU visibility and LOD selection. Timing groups contain nested totals.");
    ui.Separator();
    if (m_gpuTimingsValid)
        drawGroup("GPU stages (latest resolved sample)", m_gpuTimings);
    else
        ui.DisabledLabel("GPU stage timings are waiting for a resolved sample.");
    if(m_gpuTimingsValid)
    {
        ui.ValueLabel("Measured GPU stage total (ms)",std::to_string(m_gpuTotalMs).c_str());
        ui.ValueLabel("GPU sample / timed regions",(std::to_string(m_gpuSampleId)+" / "+std::to_string(m_gpuRegions)).c_str());
        ui.DisabledLabel("GPU samples resolve asynchronously. CPU and GPU times overlap; do not add them. GPU stage totals exclude untimed work.");
    }
    if (!m_gpuQueryTimings.empty())
        drawGroup("GPU work (CPU)", m_gpuQueryTimings);
}

void PerformanceView::Refresh(float frameRate)
{
    {
        std::lock_guard lock(m_samplerMutex);
        if (m_appliedSamplerVersion == m_samplerVersion) return;
        m_snapshot = m_pendingSnapshot;
        m_hardware = m_pendingHardware;
        m_lastCounterSampleMs = m_pendingCounterSampleMs;
        m_appliedSamplerVersion = m_samplerVersion;
    }
    RecordSnapshot(frameRate);
    PushHistory(m_cpuHistory, std::min(100.f,
        m_snapshot.cpuUser + m_snapshot.cpuKernel));
    PushHistory(m_memoryHistory, Percent(m_snapshot.memoryUsed,
        m_snapshot.memoryTotal));
    PushHistory(m_gpuHistory, std::min(100.f, std::accumulate(
        m_snapshot.gpuEngines.begin(), m_snapshot.gpuEngines.end(), 0.f)));
    PushHistory(m_gpuMemoryHistory, Percent(m_snapshot.gpuMemoryUsed,
        m_snapshot.gpuMemoryBudget));
    PushHistory(m_frameHistory, frameRate);
}

void PerformanceView::StartRecording()
{
    if (!m_recording) m_recording = std::make_shared<PerformanceRecording>();
    m_recordingError.clear();
    try
    {
        const auto directory = std::filesystem::absolute(
            std::filesystem::path(".local") / "performance-logs");
        std::filesystem::create_directories(directory);
        const auto now = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
        localtime_s(&local, &seconds);
        std::ostringstream name;
        name << "performance-" << std::put_time(&local, "%Y%m%d-%H%M%S")
            << '-' << std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() % 1000;
        const std::string prefix = name.str();
        std::filesystem::path path;
        for (unsigned suffix = 0; ; ++suffix)
        {
            path = directory / (prefix + (suffix ? "-" + std::to_string(suffix)
                : "") + ".jsonl");
            if (!std::filesystem::exists(path)) break;
        }
        if (!m_recording->Start(path))
            throw std::runtime_error("Could not create performance log file.");
        m_recordingPath = path.string();
        std::ostringstream hardware;
        hardware << "Started. CPU: " << m_hardware.cpuName
            << "; physical/logical cores: " << m_hardware.physicalCores
            << '/' << m_hardware.logicalCores
            << "; reported MHz: " << m_hardware.cpuMHz
            << "; cache bytes L1/L2/L3/L4: ";
        for (uint64_t bytes : m_hardware.cacheBytes) hardware << bytes << ' ';
        hardware << "; installed RAM bytes: " << m_hardware.installedMemory
            << "; GPU: " << m_hardware.gpuName
            << "; dedicated/shared VRAM bytes: "
            << m_hardware.dedicatedVideoMemory << '/'
            << m_hardware.sharedVideoMemory
            << "; GPU engine order: 3D, compute, copy, video decode, video encode";
        m_recording->Event("recorder", "info", hardware.str());
        m_appliedSamplerVersion = 0;
    }
    catch (const std::exception& error)
    {
        m_recordingError = error.what();
    }
}

void PerformanceView::StopRecording()
{
    if (!m_recording || !m_recording->IsRecording()) return;
    m_recording->Event("recorder", "info", "Stopped.");
    m_recording->Stop();
}

void PerformanceView::RecordSnapshot(float frameRate)
{
    if (!m_recording || !m_recording->IsRecording()) return;
    Engine::Threading::CapturedText out;
    out << "\"fps\":" << frameRate << ",\"frame_ms\":"
        << (frameRate > 0.f ? 1000.f / frameRate : 0.f)
        << ",\"counter_sample_ms\":" << m_lastCounterSampleMs
        << ",\"cpu_user_pct\":" << m_snapshot.cpuUser
        << ",\"cpu_kernel_pct\":" << m_snapshot.cpuKernel
        << ",\"cpu_scope\":\"system\",\"gpu_engines_scope\":\"system_all_adapters\""
        << ",\"process_cpu_pct\":" << m_snapshot.processCpuPercent
        << ",\"process_cpu_core_equivalents\":" << m_snapshot.processCpuCores
        << ",\"main_thread_cpu_pct\":" << m_snapshot.mainThreadCpuPercent
        << ",\"memory_total_bytes\":" << m_snapshot.memoryTotal
        << ",\"memory_used_bytes\":" << m_snapshot.memoryUsed
        << ",\"editor_working_set_bytes\":" << m_snapshot.processMemory
        << ",\"system_cache_bytes\":" << m_snapshot.systemCache
        << ",\"commit_used_bytes\":" << m_snapshot.commitUsed
        << ",\"commit_limit_bytes\":" << m_snapshot.commitLimit
        << ",\"gpu_memory_used_bytes\":" << m_snapshot.gpuMemoryUsed
        << ",\"gpu_memory_budget_bytes\":" << m_snapshot.gpuMemoryBudget
        << ",\"gpu_engines_pct\":[";
    for (size_t index = 0; index < m_snapshot.gpuEngines.size(); ++index)
        out << (index ? "," : "") << m_snapshot.gpuEngines[index];
    out << "],\"cpu_cores_pct\":[";
    for (size_t index = 0; index < m_snapshot.cores.size(); ++index)
        out << (index ? "," : "") << '[' << m_snapshot.cores[index].user
            << ',' << m_snapshot.cores[index].kernel << ']';
    out << "],\"gpu_engine_instances\":[";
    for (size_t index = 0; index < m_snapshot.gpuEngineInstances.size(); ++index)
    {
        const auto& engine = m_snapshot.gpuEngineInstances[index];
        out << (index ? "," : "") << "[\"" << engine.id << "\","
            << engine.category << ',' << engine.usage << ']';
    }
    out << ']';
    if (m_scene)
    {
        const auto& shadow = m_scene->GetShadowDebugSnapshot();
        out << ",\"shadow_enabled\":" << (shadow.enabled ? "true" : "false")
            << ",\"shadow_cascades\":" << shadow.cascadeCount
            << ",\"shadow_atlas_resolution\":" << shadow.atlasResolution
            << ",\"shadow_atlas_generation\":" << shadow.atlasAllocationGeneration
            << ",\"shadow_cascade_splits\":[" << shadow.cascadeSplits.x << ',' << shadow.cascadeSplits.y << ','
            << shadow.cascadeSplits.z << ',' << shadow.cascadeSplits.w << ']'
            << ",\"shadow_casters\":" << shadow.casterCount
            << ",\"shadow_draws\":" << shadow.submittedCasterDrawCount
            << ",\"shadow_portal_atlases\":" << shadow.portalAtlasCount
            << ",\"shadow_rendered_portal_atlases\":"
            << shadow.renderedPortalAtlasCount;
        size_t managed = 0, active = 0;
        // Hardware snapshots run at low frequency; never scan every collider
        // for every body during physics setup.
        for (const auto& object : m_scene->GetObjects())
            if (const auto* manager = object->GetComponent<Engine::Components::TerrainManager>())
            {
                managed += manager->GetManagedColliderCellCount();
                active += manager->GetActiveColliderCellCount();
            }
        out << ",\"terrain_collider_cells\":" << managed
            << ",\"terrain_active_collider_cells\":" << active;
    }
    const auto writer=m_recording->GetWriterStatistics();
    out<<",\"recording_queue_bytes\":"<<writer.queuedBytes<<",\"recording_dropped_records\":"<<writer.dropped
        <<",\"recording_failed_records\":"<<writer.failed<<",\"recording_format_ms\":"<<writer.formatNs/1.e6
        <<",\"recording_write_ms\":"<<writer.writeNs/1.e6;
    m_recording->Sample(std::move(out));
}

void PerformanceView::DrawRecording(IEditorUi& ui)
{
    ui.Label("Recording");
    const bool active = m_recording && m_recording->IsRecording();
    if (ui.Button(active ? "Stop recording" : "Start recording"))
    {
        if (active) StopRecording();
        else StartRecording();
    }
    ui.SameLine();
    ui.DisabledLabel(active ? "Recording CPU/GPU stats and editor output"
        : "Saved when recording stops");
    if(m_recording)
    {
        const auto writer=m_recording->GetWriterStatistics();
        if(active || writer.dropped || writer.failed)
            ui.ValueLabel("Writer queued / dropped / failed",(FormatBytes(writer.queuedBytes)+" / "+
                std::to_string(writer.dropped)+" / "+std::to_string(writer.failed)).c_str());
    }
    if (!m_recordingPath.empty())
    {
        ui.ValueLabel(active ? "Recording to" : "Saved performance log",
            m_recordingPath.c_str());
        if (ui.Button("Copy log path"))
            ui.SetClipboardText(m_recordingPath.c_str());
    }
    else
        ui.DisabledLabel("Logs are saved in .local/performance-logs.");
    ui.DisabledLabel("Captures frame phases, CPU/GPU stage timings, physics, viewport sizes, hardware stats, and Console/stdout/stderr. Share the .jsonl file for diagnosis.");
    if (!m_recordingError.empty())
        ui.Label(m_recordingError.c_str());
    ui.Separator();
}

void PerformanceView::DrawMetric(IEditorUi& ui, const char* id, const char* title,
    const EditorUiPercentageSegment* segments, size_t count,
    const std::deque<float>& history, EditorUiColor historyColor)
{
    float combined = 0.f;
    for (size_t index = 0; index < count; ++index)
        combined += std::max(0.f, segments[index].percentage);
    combined = std::min(combined, 100.f);
    std::ostringstream heading;
    heading << title << "   " << std::fixed << std::setprecision(1) << combined << '%';
    ui.Label(heading.str().c_str());

    if (m_visualMode == 0 || m_visualMode == 3)
        ui.PercentageGrid(id, segments, count, m_visualMode == 3 ? 100.f : 150.f);
    if (m_visualMode == 1 || m_visualMode == 3)
    {
        for (size_t index = 0; index < count; ++index)
        {
            std::ostringstream overlay;
            overlay << (segments[index].label ? segments[index].label : "Usage")
                << "  " << std::fixed << std::setprecision(1)
                << segments[index].percentage << '%';
            ui.Progress(segments[index].percentage / 100.f, overlay.str().c_str());
            const std::string detail = std::string(
                segments[index].label ? segments[index].label : "Usage") +
                ": " + std::to_string(segments[index].percentage) + "%";
            ui.Tooltip(detail.c_str());
        }
    }
    if (m_visualMode == 2 || m_visualMode == 3)
    {
        std::vector<float> values(history.begin(), history.end());
        const std::string plotId = std::string(id) + "History";
        ui.UsageHistory(plotId.c_str(), values.data(), values.size(), 0.f, 100.f,
            historyColor, 0.f, m_visualMode == 3 ? 54.f : 90.f);
    }
    ui.Spacing();
}

void PerformanceView::DrawOverview(IEditorUi& ui, float frameRate)
{
    const char* modes[] = { "10x10 category grids", "Category bars",
        "Scrolling timelines", "All visuals" };
    ui.Combo("Visual", &m_visualMode, modes, static_cast<int>(std::size(modes)));
    std::ostringstream frame;
    frame << std::fixed << std::setprecision(1) << frameRate << " FPS  |  "
        << (frameRate > 0.f ? 1000.f / frameRate : 0.f) << " ms";
    ui.ValueLabel("Editor FPS (0.5 s)", frame.str().c_str());
    if (m_visualMode == 2 || m_visualMode == 3)
    {
        std::vector<float> frames(m_frameHistory.begin(), m_frameHistory.end());
        ui.UsageHistory("##frameHistory", frames.data(), frames.size(), 0.f,
            std::max(144.f, frames.empty() ? 144.f : *std::max_element(frames.begin(), frames.end())),
            { .95f, .78f, .24f, 1.f }, 0.f, 54.f);
    }
    ui.Separator();

    const EditorUiPercentageSegment cpu[] = {
        { "User", m_snapshot.cpuUser, kUserColor },
        { "Kernel", m_snapshot.cpuKernel, kKernelColor }
    };
    DrawMetric(ui, "##cpuGrid", "System CPU", cpu, std::size(cpu), m_cpuHistory, kUserColor);

    ui.ValueLabel("Editor CPU (% of system)",std::to_string(m_snapshot.processCpuPercent).c_str());
    ui.ValueLabel("Editor CPU (logical-core equivalents)",std::to_string(m_snapshot.processCpuCores).c_str());
    ui.ValueLabel("Main thread CPU (% of one logical core)",std::to_string(m_snapshot.mainThreadCpuPercent).c_str());
    const float process = Percent(m_snapshot.processMemory, m_snapshot.memoryTotal);
    const float cache = Percent(std::min(m_snapshot.systemCache,
        m_snapshot.memoryUsed > m_snapshot.processMemory
            ? m_snapshot.memoryUsed - m_snapshot.processMemory : 0), m_snapshot.memoryTotal);
    const float used = Percent(m_snapshot.memoryUsed, m_snapshot.memoryTotal);
    const EditorUiPercentageSegment memory[] = {
        { "Editor working set", process, kProcessColor },
        { "System cache", cache, kCacheColor },
        { "Other used", std::max(0.f, used - process - cache), kOtherMemoryColor }
    };
    DrawMetric(ui, "##memoryGrid", "Physical memory", memory, std::size(memory),
        m_memoryHistory, kProcessColor);
    const std::string memoryText = FormatBytes(m_snapshot.memoryUsed) + " / " +
        FormatBytes(m_snapshot.memoryTotal) + "   Editor " +
        FormatBytes(m_snapshot.processMemory) + "   Cache " +
        FormatBytes(m_snapshot.systemCache);
    ui.DisabledLabel(memoryText.c_str());

    std::array<EditorUiPercentageSegment, 6> gpu{};
    for (size_t index = 0; index < gpu.size(); ++index)
        gpu[index] = { kGpuLabels[index], m_snapshot.gpuEngines[index], kGpuColors[index] };
    DrawMetric(ui, "##gpuGrid", "Busiest GPU engines by category (all adapters)", gpu.data(), gpu.size(),
        m_gpuHistory, kGpuColors[0]);
    if (m_snapshot.gpuMemoryBudget)
    {
        const EditorUiPercentageSegment videoMemory[] = {
            { "Local memory", Percent(m_snapshot.gpuMemoryUsed,
                m_snapshot.gpuMemoryBudget), kGpuColors[4] }
        };
        DrawMetric(ui, "##vramGrid", "Editor GPU memory (selected adapter)", videoMemory, 1,
            m_gpuMemoryHistory, kGpuColors[4]);
        const std::string videoText = FormatBytes(m_snapshot.gpuMemoryUsed) + " / " +
            FormatBytes(m_snapshot.gpuMemoryBudget) + " current OS budget";
        ui.DisabledLabel(videoText.c_str());
    }
    ui.DisabledLabel("GPU engine activity includes all applications and adapters. Category bars show the busiest engine. GPU memory is editor usage on the selected adapter.");
}

void PerformanceView::DrawCoreGrid(IEditorUi& ui)
{
    if (m_snapshot.cores.empty())
    {
        ui.DisabledLabel("Per-core counters are not available yet.");
        return;
    }
    const int columns = std::max(1, static_cast<int>(ui.AvailableContentWidth() / 92.f));
    if (!ui.BeginTable("##coreTiles", columns)) return;
    for (size_t index = 0; index < m_snapshot.cores.size(); ++index)
    {
        ui.TableNextColumn();
        const float usage = std::min(100.f,
            m_snapshot.cores[index].user + m_snapshot.cores[index].kernel);
        std::ostringstream label;
        label << "CPU " << index << "  " << std::fixed << std::setprecision(0)
            << usage << '%';
        ui.Label(label.str().c_str());
        const EditorUiPercentageSegment core[] = {
            { "User", m_snapshot.cores[index].user, kUserColor },
            { "Kernel", m_snapshot.cores[index].kernel, kKernelColor }
        };
        const std::string id = "##core" + std::to_string(index);
        ui.PercentageGrid(id.c_str(), core, std::size(core), 66.f);
    }
    ui.EndTable();
}

void PerformanceView::DrawGpuEngineGrid(IEditorUi& ui)
{
    if (m_snapshot.gpuEngineInstances.empty())
    {
        ui.DisabledLabel("Per-engine GPU counters are unavailable.");
        return;
    }
    ui.DisabledLabel("System usage by physical GPU engine, including all processes. Adapter LUID and engine index identify each engine.");
    const int columns = std::max(1, static_cast<int>(ui.AvailableContentWidth() / 220.f));
    if (!ui.BeginTable("##gpuEngineTiles", columns)) return;
    for (size_t index = 0; index < m_snapshot.gpuEngineInstances.size(); ++index)
    {
        const auto& engine = m_snapshot.gpuEngineInstances[index];
        ui.TableNextColumn();
        const std::string label = std::string(kGpuLabels[engine.category]) + "  " +
            engine.id + "  " + std::to_string(static_cast<int>(std::round(engine.usage))) + '%';
        ui.Label(label.c_str());
        const EditorUiPercentageSegment segment[] = {
            { kGpuLabels[engine.category], engine.usage, kGpuColors[engine.category] }
        };
        const std::string id = "##gpuEngine" + std::to_string(index);
        ui.PercentageGrid(id.c_str(), segment, 1, 66.f);
    }
    ui.EndTable();
}

void PerformanceView::DrawHardware(IEditorUi& ui)
{
    ui.ValueLabel("Processor", m_hardware.cpuName.c_str());
    const std::string cores = std::to_string(m_hardware.physicalCores) +
        " physical / " + std::to_string(m_hardware.logicalCores) + " logical";
    ui.ValueLabel("Cores", cores.c_str());
    if (m_hardware.cpuMHz)
    {
        const std::string speed = std::to_string(m_hardware.cpuMHz) + " MHz reported";
        ui.ValueLabel("Clock", speed.c_str());
    }
    for (size_t index = 0; index < m_hardware.cacheBytes.size(); ++index)
        if (m_hardware.cacheBytes[index])
        {
            const std::string label = "L" + std::to_string(index + 1) + " cache";
            const std::string value = FormatBytes(m_hardware.cacheBytes[index]);
            ui.ValueLabel(label.c_str(), value.c_str());
        }
    ui.Separator();
    ui.ValueLabel("Installed RAM", FormatBytes(m_hardware.installedMemory).c_str());
    ui.ValueLabel("Commit", (FormatBytes(m_snapshot.commitUsed) + " / " +
        FormatBytes(m_snapshot.commitLimit)).c_str());
    ui.Separator();
    ui.ValueLabel("Graphics adapter", m_hardware.gpuName.c_str());
    ui.ValueLabel("Dedicated video memory",
        FormatBytes(m_hardware.dedicatedVideoMemory).c_str());
    ui.ValueLabel("Shared video memory",
        FormatBytes(m_hardware.sharedVideoMemory).c_str());
}

void PerformanceView::DrawShadows(IEditorUi& ui)
{
    if (!m_scene)
    {
        ui.DisabledLabel("No active scene is available.");
        return;
    }

    const Engine::Scene::Scene::ShadowDebugSnapshot& shadow =
        m_scene->GetShadowDebugSnapshot();
    ui.ValueLabel("Realtime shadows", shadow.enabled ? "Enabled" : "Disabled");
    if (!shadow.enabled)
    {
        ui.DisabledLabel("Enable realtime shadows in project preferences to collect diagnostics.");
        return;
    }

    ui.Separator();
    ui.Label("Selected shadow light");
    if (!shadow.hasSelectedLight)
    {
        ui.DisabledLabel("No eligible realtime directional light was selected.");
    }
    else
    {
        ui.ValueLabel("Light", shadow.selectedLight.c_str());
        std::ostringstream direction;
        direction << std::fixed << std::setprecision(3)
            << shadow.directionToLight.x << ", "
            << shadow.directionToLight.y << ", "
            << shadow.directionToLight.z;
        ui.ValueLabel("Direction to light", direction.str().c_str());
    }

    ui.Separator();
    ui.Label("Directional shadow atlas");
    if (!shadow.hasSelectedLight || !shadow.atlasResolution ||
        !shadow.cascadeCount)
    {
        ui.DisabledLabel("No directional shadow atlas is allocated.");
    }
    else
    {
        const std::string resolution = std::to_string(shadow.atlasResolution) +
            " x " + std::to_string(shadow.atlasResolution);
        ui.ValueLabel("Resolution", resolution.c_str());
        const uint32_t tileCount = shadow.cascadeCount == 1u ? 1u : 4u;
        const float occupancy = 100.f * static_cast<float>(
            shadow.cascadeCount) / static_cast<float>(tileCount);
        std::ostringstream occupancyText;
        occupancyText << std::fixed << std::setprecision(0) << occupancy
            << "% (" << shadow.cascadeCount << " / " << tileCount
            << " tiles)";
        ui.ValueLabel("Occupancy", occupancyText.str().c_str());
        ui.ValueLabel("Allocation generation",
            std::to_string(shadow.atlasAllocationGeneration).c_str());

        constexpr std::array<EditorUiColor, 4> cascadeColors{{
            { .18f, .72f, 1.f, 1.f }, { .30f, .84f, .48f, 1.f },
            { .96f, .68f, .22f, 1.f }, { .90f, .34f, .52f, 1.f }
        }};
        const int columns = tileCount == 1u ? 1 : 2;
        if (ui.BeginTable("##shadowAtlas", columns))
        {
            for (uint32_t tile = 0; tile < tileCount; ++tile)
            {
                ui.TableNextColumn();
                if (tile < shadow.cascadeCount)
                {
                    const std::string title = "Cascade " +
                        std::to_string(tile + 1u);
                    ui.Label(title.c_str());
                    const EditorUiPercentageSegment segment[] = {
                        { title.c_str(), 100.f, cascadeColors[tile] }
                    };
                    const std::string id = "##shadowTile" +
                        std::to_string(tile);
                    ui.PercentageGrid(id.c_str(), segment, 1u, 72.f);
                }
                else
                {
                    ui.DisabledLabel("Unused tile");
                    ui.Spacing();
                }
            }
            ui.EndTable();
        }
    }

    ui.Separator();
    ui.Label("Cascade bounds");
    if (!shadow.cascadeCount)
        ui.DisabledLabel("No cascade bounds are available.");
    for (uint32_t cascade = 0; cascade < shadow.cascadeCount; ++cascade)
    {
        std::ostringstream label;
        label << "Cascade " << cascade + 1u << "  0-" << std::fixed
            << std::setprecision(2) << shadow.cascadeSplits[cascade];
        ui.Label(label.str().c_str());
        const glm::vec3& center = shadow.cascadeCenters[cascade];
        std::ostringstream bounds;
        bounds << "Center (" << std::fixed << std::setprecision(2)
            << center.x << ", " << center.y << ", " << center.z
            << ")  Radius " << shadow.cascadeRadii[cascade]
            << "  Diameter " << shadow.cascadeRadii[cascade] * 2.f;
        ui.DisabledLabel(bounds.str().c_str());
    }

    ui.Separator();
    ui.Label("Shadow workload");
    ui.ValueLabel("Unique eligible casters",
        std::to_string(shadow.casterCount).c_str());
    ui.ValueLabel("Main cascade caster draws",
        std::to_string(shadow.submittedCasterDrawCount).c_str());
    ui.ValueLabel("Static baked receivers",
        std::to_string(shadow.bakedReceiverCount).c_str());
    ui.ValueLabel("Dynamic realtime receivers",
        std::to_string(shadow.dynamicReceiverCount).c_str());
    ui.DisabledLabel("Baked receivers skip realtime shadow sampling but remain realtime casters for dynamic objects.");

    ui.Separator();
    ui.Label("Portal shadow atlases");
    ui.ValueLabel("Allocated / rendered",
        (std::to_string(shadow.portalAtlasCount) + " / " +
            std::to_string(shadow.renderedPortalAtlasCount)).c_str());
    if (shadow.portalAtlasResolution)
    {
        const std::string portalResolution =
            std::to_string(shadow.portalAtlasResolution) + " x " +
            std::to_string(shadow.portalAtlasResolution);
        ui.ValueLabel("Per-view resolution", portalResolution.c_str());
    }
    else
        ui.DisabledLabel("Portal views reuse the main directional atlas.");
}

void PerformanceView::DrawPanel(IEditorUi& ui)
{
    const auto drawStart = std::chrono::steady_clock::now();
    m_lastUiFrameRate=ui.FrameRate();
    const float frameRate = m_displayFrameRate;
    const auto finishDraw = [&] {
        m_lastPanelDrawMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - drawStart).count();
    };
    const bool sampleActive = m_open ||
        (m_recording && m_recording->IsRecording());
    bool wakeSampler = false;
    {
        std::lock_guard lock(m_samplerMutex);
        if (m_samplerActive != sampleActive)
        {
            m_samplerActive = sampleActive;
            wakeSampler = true;
        }
    }
    if (wakeSampler) m_samplerWake.notify_one();
    if (sampleActive) Refresh(frameRate);
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        finishDraw();
        return;
    }
    DrawRecording(ui);
    if (ui.BeginTabBar("##performanceTabs"))
    {
        if (ui.BeginTab("Overview"))
        {
            DrawOverview(ui, frameRate);
            ui.EndTab();
        }
        if (ui.BeginTab("Timings"))
        {
            DrawTimings(ui);
            ui.EndTab();
        }
        if (ui.BeginTab("Logical cores"))
        {
            DrawCoreGrid(ui);
            ui.EndTab();
        }
        if (ui.BeginTab("GPU engines"))
        {
            DrawGpuEngineGrid(ui);
            ui.EndTab();
        }
        if (ui.BeginTab("Hardware"))
        {
            DrawHardware(ui);
            ui.EndTab();
        }
        if (ui.BeginTab("Shadows"))
        {
            DrawShadows(ui);
            ui.EndTab();
        }
        ui.EndTabBar();
    }
    ui.EndWindow();
    finishDraw();
}
}
