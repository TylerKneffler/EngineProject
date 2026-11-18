#include "Core/Scene/SceneCounters.h"


namespace Engine::Scene
{
void SceneCounters::Reset()
{
    for (auto& counter : m_counters) counter.value = 0;
    ++m_frame;
}

SceneCounter& SceneCounters::Declare(std::string_view key, std::string_view group,
    std::string_view label, CounterUnit unit)
{
    const auto [found, inserted] = m_index.try_emplace(std::string(key), m_counters.size());
    if (inserted)
        m_counters.push_back({ std::string(key), std::string(group), std::string(label), unit, 0 });
    return m_counters[found->second];
}

void SceneCounters::Add(std::string_view key, std::string_view group,
    std::string_view label, CounterUnit unit, double value)
{
    Declare(key, group, label, unit).value += value;
}

void SceneCounters::Add(const std::vector<SceneCounter>& values)
{
    for (const auto& counter : values)
        Add(counter.key, counter.group, counter.label, counter.unit, counter.value);
}
}
