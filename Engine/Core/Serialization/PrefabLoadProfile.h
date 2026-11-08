#pragma once
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace Engine::Serialization
{
// Opt-in timings for interactive placement and standalone prefab benchmarks.
class PrefabLoadProfile
{
    using Clock = std::chrono::steady_clock;
    struct Entry { double milliseconds = 0; size_t calls = 0; };
    inline static thread_local PrefabLoadProfile* current = nullptr;
    std::map<std::string, Entry> entries;
    std::string path;
    Clock::time_point started = Clock::now();
    bool owner = false;
public:
    explicit PrefabLoadProfile(const std::string& assetPath) : path(assetPath)
    {
        if (current) return;
#ifdef _MSC_VER
        char* value = nullptr;
        size_t length = 0;
        const bool enabled = _dupenv_s(&value, &length,
            "ENGINE_PREFAB_LOAD_PROFILE") == 0 && value != nullptr;
        std::free(value);
#else
        const bool enabled = std::getenv("ENGINE_PREFAB_LOAD_PROFILE") != nullptr;
#endif
        if (enabled)
        { current = this; owner = true; }
    }
    ~PrefabLoadProfile()
    {
        if (!owner) return;
        current = nullptr;
        std::ofstream log("prefab-load-profile.log", std::ios::app);
        log << "Prefab: " << path << "\ntotal_ms="
            << std::chrono::duration<double, std::milli>(Clock::now() - started).count() << '\n';
        for (const auto& [name, entry] : entries)
            log << name << " ms=" << entry.milliseconds << " calls=" << entry.calls << '\n';
        log << '\n';
    }
    inline static thread_local std::string activeStage;
    static const std::string& ActiveStage() { return activeStage; }
    class Stage
    {
        PrefabLoadProfile* profile = current;
        std::string name;
        std::string previous;
        Clock::time_point started = Clock::now();
    public:
        explicit Stage(const std::string& label) : name(profile ? label : "")
        {
            if (profile) { previous = std::move(activeStage); activeStage = name; }
        }
        ~Stage()
        {
            if (!profile) return;
            activeStage = std::move(previous);
            auto& entry = profile->entries[name];
            entry.milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            ++entry.calls;
        }
    };
    static bool Enabled() { return current != nullptr; }
};
}
