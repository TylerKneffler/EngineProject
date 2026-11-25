#include "pch.h"
#include "Core/Graphics/ShaderCompileProgress.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace Engine::Graphics
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Active
{
    std::string key, label;
    Clock::time_point start;
};

struct State
{
    std::mutex mutex;
    std::map<int, Active> active;
    int nextTicket = 1;
    int finished = 0;
    Clock::time_point lastEnd = Clock::now();
    std::filesystem::path store;
    bool loaded = false;
    std::map<std::string, double> durations;   // key -> seconds
};

State& Shared()
{
    static State state;
    return state;
}

// "Object.hlsl PSMain ps_5_0" -> seconds, one per line (tab separated).
void LoadDurations(State& state)
{
    if (state.loaded) return;
    state.loaded = true;
    if (state.store.empty()) return;
    std::ifstream stream(state.store);
    std::string line;
    while (std::getline(stream, line))
    {
        const auto tab = line.rfind('\t');
        if (tab == std::string::npos) continue;
        try { state.durations[line.substr(0, tab)] = std::stod(line.substr(tab + 1)); }
        catch (...) {}
    }
}

void SaveDurations(const State& state)
{
    if (state.store.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(state.store.parent_path(), error);
    // Other processes may write it too: a whole-file rewrite through a
    // temporary, last writer wins (it is only an estimate).
    std::ostringstream suffix;
    suffix << ".tmp" << GetCurrentProcessId();
    const auto temporary = std::filesystem::path(state.store.string() + suffix.str());
    {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!stream) return;
        for (const auto& [key, seconds] : state.durations)
            stream << key << '\t' << seconds << '\n';
    }
    std::filesystem::rename(temporary, state.store, error);
    if (error) std::filesystem::remove(temporary, error);
}
}

void ShaderCompileProgress::SetStoreDirectory(const std::string& directory)
{
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    if (state.loaded) return;
    state.store = directory.empty() ? std::filesystem::path()
        : std::filesystem::path(directory) / "compile_times.txt";
}

int ShaderCompileProgress::Begin(const std::string& filePath, const char* entryPoint, const char* target)
{
    auto& state = Shared();
    const std::string file = std::filesystem::path(filePath).filename().string();
    Active entry;
    entry.label = file + " " + (entryPoint ? entryPoint : "");
    entry.key = entry.label + " " + (target ? target : "");
    entry.start = Clock::now();
    std::lock_guard lock(state.mutex);
    LoadDurations(state);
    const int ticket = state.nextTicket++;
    state.active.emplace(ticket, std::move(entry));
    return ticket;
}

void ShaderCompileProgress::End(int ticket, bool succeeded)
{
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    const auto found = state.active.find(ticket);
    if (found == state.active.end()) return;
    const auto now = Clock::now();
    if (succeeded)
    {
        state.durations[found->second.key] =
            std::chrono::duration<double>(now - found->second.start).count();
        SaveDurations(state);
    }
    state.active.erase(found);
    ++state.finished;
    state.lastEnd = now;
}

ShaderCompileProgress::Snapshot ShaderCompileProgress::Get()
{
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    Snapshot snapshot;
    snapshot.active = static_cast<int>(state.active.size());
    snapshot.finished = state.finished;
    const auto now = Clock::now();
    const Active* oldest = nullptr;
    for (const auto& [ticket, entry] : state.active)
        if (!oldest || entry.start < oldest->start) oldest = &entry;
    if (oldest)
    {
        snapshot.label = oldest->label;
        snapshot.elapsedSeconds = std::chrono::duration<double>(now - oldest->start).count();
        if (const auto known = state.durations.find(oldest->key); known != state.durations.end())
            snapshot.expectedSeconds = known->second;
    }
    else
        snapshot.idleSeconds = std::chrono::duration<double>(now - state.lastEnd).count();
    return snapshot;
}
}
