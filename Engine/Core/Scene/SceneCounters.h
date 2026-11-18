#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Engine::Scene
{
enum class CounterUnit : uint8_t { Milliseconds, Count, Bytes };

// One named per-frame diagnostic value. The key is its JSON name in profiler
// and benchmark output; group and label place it in the Performance view.
struct SceneCounter
{
    std::string key;
    std::string group;
    std::string label;
    CounterUnit unit = CounterUnit::Count;
    double value = 0;
};

// Per-frame counters that scripts and plugins publish without the host
// knowing what they measure (GrassField publishes "grass_*"). The host loop
// resets them once per frame (GameApplication, EditorApplication); the
// Performance view, editor profiler and game benchmark list every counter in
// declaration order. Owned by the Scene, so scripts in a hot-reloaded module
// share it with the host. Scene owner thread only.
class SceneCounters
{
public:
    // Zeroes every value (declarations stay, keeping display order) and
    // starts a new frame.
    void Reset();
    // Adds to a counter, declaring it on first use.
    void Add(std::string_view key, std::string_view group, std::string_view label,
        CounterUnit unit, double value);
    // Adds counters captured from Get() earlier, matched by key.
    void Add(const std::vector<SceneCounter>& values);
    const std::vector<SceneCounter>& Get() const { return m_counters; }
    // Increments on every Reset, so publishers can keep their own per-frame
    // state without a reset hook.
    uint64_t GetFrame() const { return m_frame; }

private:
    SceneCounter& Declare(std::string_view key, std::string_view group,
        std::string_view label, CounterUnit unit);

    std::vector<SceneCounter> m_counters;
    std::unordered_map<std::string, size_t> m_index;
    uint64_t m_frame = 0;
};

// Writes `,"key":value` for each counter (counts and bytes as integers) to
// any stream-like output (std::ostream, CapturedText).
template<class Output>
void WriteCountersJson(Output& output, const std::vector<SceneCounter>& counters)
{
    for (const auto& counter : counters)
    {
        output << ",\"" << counter.key << "\":";
        if (counter.unit == CounterUnit::Milliseconds) output << counter.value;
        else output << static_cast<uint64_t>(counter.value);
    }
}
}
