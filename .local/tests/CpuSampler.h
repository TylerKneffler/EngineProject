#pragma once
// In-process sampling profiler for GameBackendCapture (no admin/ETW needed).
// A sampler thread suspends every other thread about once per millisecond,
// unwinds its stack into preallocated storage, resumes it, then aggregates.
// Nothing allocates while a target thread is suspended (it may hold the heap
// lock). Symbols resolve from the PDBs at report time.
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

class CpuSampler
{
public:
    // Samples between Begin() and End() (callable from any thread).
    CpuSampler() { m_thread = std::thread([this] { Run(); }); }
    void Begin() { m_active = true; }
    void End() { m_active = false; m_stop = true; }
    ~CpuSampler() { Stop(); }

    void Stop()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }

    // Writes per-thread inclusive/exclusive tables for the busiest threads.
    void Report(const std::filesystem::path& path, size_t threads = 6, size_t rows = 70)
    {
        Stop();
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        std::unordered_map<uint64_t, std::string> names;
        const auto name = [&](uint64_t address) -> const std::string& {
            auto found = names.find(address);
            if (found != names.end()) return found->second;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            std::string text;
            if (SymFromAddr(process, address, &displacement, symbol)) text = symbol->Name;
            else
            {
                char module[MAX_PATH]{};
                HMODULE handle = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address), &handle))
                    GetModuleFileNameA(handle, module, MAX_PATH);
                text = std::string("?? ") + std::filesystem::path(module).filename().string();
            }
            return names.emplace(address, std::move(text)).first->second;
        };

        std::map<DWORD, std::vector<const Sample*>> byThread;
        for (const auto& sample : m_samples) byThread[sample.thread].push_back(&sample);
        // Rank threads by busy (non-waiting) samples.
        std::vector<std::pair<DWORD, size_t>> order;
        for (auto& [thread, samples] : byThread)
        {
            size_t busy = 0;
            for (const auto* sample : samples)
                if (sample->depth && !IsIdle(name(sample->frames[0]))) ++busy;
            order.push_back({thread, busy});
        }
        std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second > b.second; });

        std::ofstream out(path);
        out << "CPU samples: " << m_samples.size() << " over " << m_sampledSeconds
            << " s (interval ~1 ms). Busy samples only (idle waits excluded).\n";
        for (size_t t = 0; t < std::min(threads, order.size()); ++t)
        {
            const auto& samples = byThread[order[t].first];
            std::unordered_map<std::string, size_t> inclusive, exclusive;
            size_t busy = 0;
            for (const auto* sample : samples)
            {
                if (sample->depth == 0) continue;
                const std::string& leaf = name(sample->frames[0]);
                if (IsIdle(leaf)) continue;
                ++busy;
                ++exclusive[leaf];
                std::unordered_set<std::string> seen;
                for (uint32_t i = 0; i < sample->depth; ++i)
                {
                    const std::string& frame = name(sample->frames[i]);
                    if (seen.insert(frame).second) ++inclusive[frame];
                }
            }
            out << "\n=== thread " << order[t].first << ": " << samples.size()
                << " samples, " << busy << " busy ===\n";
            const auto print = [&](const char* title, auto& table) {
                std::vector<std::pair<std::string, size_t>> sorted(table.begin(), table.end());
                std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
                out << "-- " << title << " (% of busy)\n";
                for (size_t i = 0; i < std::min(rows, sorted.size()); ++i)
                {
                    char line[64];
                    snprintf(line, sizeof(line), "%6.2f%%  ", busy ? 100.0 * sorted[i].second / busy : 0.0);
                    out << line << sorted[i].first << '\n';
                }
            };
            print("inclusive", inclusive);
            print("exclusive", exclusive);
            // Caller chains for the hottest functions (inclusive, skipping
            // generic frames), so leaf costs can be attributed to call sites.
            std::vector<std::pair<std::string, size_t>> hot(inclusive.begin(), inclusive.end());
            std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.second > b.second; });
            size_t shown = 0;
            for (const auto& [function, count] : hot)
            {
                if (shown >= 25 || count * 100 < busy) break;
                if (function.find("Thread") != std::string::npos || function.find("_Invoke") != std::string::npos ||
                    function == "wcsrchr" || function.find("::Run") != std::string::npos) continue;
                ++shown;
                std::unordered_map<std::string, size_t> callers;
                for (const auto* sample : samples)
                    for (uint32_t i = 0; i + 1 < sample->depth; ++i)
                        if (name(sample->frames[i]) == function)
                        {
                            std::string chain = name(sample->frames[i + 1]);
                            if (i + 2 < sample->depth) chain += "  <-  " + name(sample->frames[i + 2]);
                            ++callers[chain];
                            break;
                        }
                std::vector<std::pair<std::string, size_t>> sorted(callers.begin(), callers.end());
                std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
                out << "-- callers of " << function.substr(0, 120) << '\n';
                for (size_t i = 0; i < std::min<size_t>(6, sorted.size()); ++i)
                {
                    char line[64];
                    snprintf(line, sizeof(line), "   %6.2f%%  ", 100.0 * sorted[i].second / busy);
                    out << line << sorted[i].first.substr(0, 240) << '\n';
                }
            }
        }
        SymCleanup(process);
    }

private:
    static constexpr uint32_t kMaxDepth = 48;
    struct Sample
    {
        DWORD thread = 0;
        uint32_t depth = 0;
        uint64_t frames[kMaxDepth]{};
    };

    static bool IsIdle(const std::string& leaf)
    {
        // Threads blocked in the kernel: waits, sleeps, message pumps, present.
        static const char* idle[] = { "NtWaitFor", "ZwWaitFor", "NtDelayExecution",
            "NtRemoveIoCompletion", "NtUserMsgWaitForMultipleObjects", "NtUserGetMessage",
            "NtUserPeekMessage", "NtGdiDdDDIWaitForSynchronizationObject",
            "ZwRemoveIoCompletion", "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId",
            "NtDelayExecution", "ZwDelayExecution", "NtGdiDdDDIPresent" };
        for (const char* prefix : idle)
            if (leaf.rfind(prefix, 0) == 0) return true;
        return false;
    }

    static uint32_t Unwind(CONTEXT context, uint64_t* frames)
    {
        uint32_t depth = 0;
        while (depth < kMaxDepth && context.Rip)
        {
            frames[depth++] = context.Rip;
            DWORD64 imageBase = 0;
            auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                // Leaf function: return address is at RSP.
                if (!context.Rsp) break;
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function,
                &context, &handlerData, &establisher, nullptr);
        }
        return depth;
    }

    void Run()
    {
        auto begin = std::chrono::steady_clock::time_point{};
        const DWORD self = GetCurrentThreadId();
        const DWORD process = GetCurrentProcessId();
        m_samples.reserve(500000); // ~200 MB; reserved before sampling
        std::vector<std::pair<DWORD, HANDLE>> threads;
        auto lastEnumeration = std::chrono::steady_clock::time_point{};
        timeBeginPeriod(1);
        while (!m_stop)
        {
            if (!m_active) { Sleep(5); continue; }
            if (begin == std::chrono::steady_clock::time_point{})
                begin = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begin).count();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEnumeration > std::chrono::milliseconds(500))
            {
                for (auto& [id, handle] : threads) CloseHandle(handle);
                threads.clear();
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{ sizeof(entry) };
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
                        if (HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID))
                            threads.push_back({ entry.th32ThreadID, handle });
                CloseHandle(snapshot);
                lastEnumeration = now;
            }
            for (auto& [id, handle] : threads)
            {
                if (m_samples.size() == m_samples.capacity()) { m_stop = true; break; }
                Sample sample;
                sample.thread = id;
                if (SuspendThread(handle) == DWORD(-1)) continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(handle, &context))
                    sample.depth = Unwind(context, sample.frames);
                ResumeThread(handle);
                m_samples.push_back(sample); // capacity reserved: no allocation
            }
            m_sampledSeconds = elapsed;
            Sleep(1);
        }
        timeEndPeriod(1);
        for (auto& [id, handle] : threads) CloseHandle(handle);
    }

    double m_sampledSeconds = 0;
    std::atomic<bool> m_stop{ false }, m_active{ false };
    std::vector<Sample> m_samples;
    std::thread m_thread;
};
