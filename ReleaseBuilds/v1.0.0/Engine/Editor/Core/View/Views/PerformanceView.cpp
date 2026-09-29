#include "PerformanceView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
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
constexpr std::array<EditorUiColor, 5> kGpuColors{{
    { .30f, .82f, .42f, 1.f }, { .10f, .67f, .75f, 1.f },
    { .95f, .58f, .18f, 1.f }, { .90f, .34f, .50f, 1.f },
    { .62f, .45f, .92f, 1.f }
}};
constexpr const char* kGpuLabels[] = {
    "3D", "Compute", "Copy", "Video decode", "Video encode"
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
    Sampler()
    {
        QueryHardware();
        QueryProcessorTimes(m_previousProcessors);
        InitializeGpuCounters();
    }

    ~Sampler()
    {
        if (m_gpuQuery) PdhCloseQuery(m_gpuQuery);
    }

    const PerformanceHardware& Hardware() const { return m_hardware; }

    PerformanceSnapshot Sample()
    {
        PerformanceSnapshot result;
        SampleProcessors(result);
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
    struct GpuCounter { PDH_HCOUNTER handle = nullptr; int category = 0; };

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
        DWORD length = 0;
        PdhExpandWildCardPathW(nullptr,
            L"\\GPU Engine(*)\\Utilization Percentage", nullptr, &length,
            PDH_NOEXPANDCOUNTERS);
        if (!length) return;
        std::vector<wchar_t> paths(length);
        if (PdhExpandWildCardPathW(nullptr,
            L"\\GPU Engine(*)\\Utilization Percentage", paths.data(), &length,
            PDH_NOEXPANDCOUNTERS) != ERROR_SUCCESS) return;
        for (const wchar_t* path = paths.data(); *path; path += wcslen(path) + 1)
        {
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddEnglishCounterW(m_gpuQuery, path, 0, &counter) != ERROR_SUCCESS)
                continue;
            std::wstring lower(path);
            std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            int category = 0;
            if (lower.find(L"engtype_compute") != std::wstring::npos) category = 1;
            else if (lower.find(L"engtype_copy") != std::wstring::npos) category = 2;
            else if (lower.find(L"engtype_video decode") != std::wstring::npos) category = 3;
            else if (lower.find(L"engtype_video encode") != std::wstring::npos) category = 4;
            m_gpuCounters.push_back({ counter, category });
        }
        if (!m_gpuCounters.empty()) PdhCollectQueryData(m_gpuQuery);
    }

    void SampleGpu(PerformanceSnapshot& result)
    {
        if (m_gpuQuery && PdhCollectQueryData(m_gpuQuery) == ERROR_SUCCESS)
        {
            for (const GpuCounter& counter : m_gpuCounters)
            {
                PDH_FMT_COUNTERVALUE value{};
                if (PdhGetFormattedCounterValue(counter.handle, PDH_FMT_DOUBLE,
                    nullptr, &value) == ERROR_SUCCESS &&
                    value.CStatus == PDH_CSTATUS_VALID_DATA && std::isfinite(value.doubleValue))
                    result.gpuEngines[counter.category] +=
                        static_cast<float>(std::max(0.0, value.doubleValue));
            }
            for (float& usage : result.gpuEngines)
                usage = std::clamp(usage, 0.f, 100.f);
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
    ComPtr<IDXGIAdapter3> m_adapter;
};

PerformanceView::PerformanceView()
    : m_sampler(std::make_unique<Sampler>())
{
    SetCursorBehaviorOnFocus(CursorBehaviorOnFocus::Visible);
    m_hardware = m_sampler->Hardware();
}

PerformanceView::~PerformanceView() = default;

void PerformanceView::Refresh(float frameRate)
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextSample) return;
    m_nextSample = now + std::chrono::milliseconds(500);
    m_snapshot = m_sampler->Sample();
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
    ui.ValueLabel("Frame", frame.str().c_str());
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
    DrawMetric(ui, "##cpuGrid", "CPU", cpu, std::size(cpu), m_cpuHistory, kUserColor);

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

    std::array<EditorUiPercentageSegment, 5> gpu{};
    for (size_t index = 0; index < gpu.size(); ++index)
        gpu[index] = { kGpuLabels[index], m_snapshot.gpuEngines[index], kGpuColors[index] };
    DrawMetric(ui, "##gpuGrid", "GPU engines", gpu.data(), gpu.size(),
        m_gpuHistory, kGpuColors[0]);
    if (m_snapshot.gpuMemoryBudget)
    {
        const EditorUiPercentageSegment videoMemory[] = {
            { "Local memory", Percent(m_snapshot.gpuMemoryUsed,
                m_snapshot.gpuMemoryBudget), kGpuColors[4] }
        };
        DrawMetric(ui, "##vramGrid", "GPU memory", videoMemory, 1,
            m_gpuMemoryHistory, kGpuColors[4]);
        const std::string videoText = FormatBytes(m_snapshot.gpuMemoryUsed) + " / " +
            FormatBytes(m_snapshot.gpuMemoryBudget) + " current OS budget";
        ui.DisabledLabel(videoText.c_str());
    }
    ui.DisabledLabel("GPU engine categories may execute concurrently; combined occupancy is capped at 100%.");
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

void PerformanceView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    const float frameRate = ui.FrameRate();
    Refresh(frameRate);
    if (ui.BeginTabBar("##performanceTabs"))
    {
        if (ui.BeginTab("Overview"))
        {
            DrawOverview(ui, frameRate);
            ui.EndTab();
        }
        if (ui.BeginTab("Logical cores"))
        {
            DrawCoreGrid(ui);
            ui.EndTab();
        }
        if (ui.BeginTab("Hardware"))
        {
            DrawHardware(ui);
            ui.EndTab();
        }
        ui.EndTabBar();
    }
    ui.EndWindow();
}
}
