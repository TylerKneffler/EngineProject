#include "Core/Components/UI/UILineGraph.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Engine::Components
{
UILineGraph::UILineGraph()
{
    SetTypeName(COMPONENT_TYPE_NAME(UILineGraph));
    singlecomponent = true;
    RegisterField("sampleCount", sampleCount, "Graph");
    RegisterField("minimum", minimum, "Graph");
    RegisterField("maximum", maximum, "Graph");
    RegisterField("autoRange", autoRange, "Graph");
    RegisterField("lineWidth", lineWidth, "Appearance");
    RegisterField("backgroundColor", backgroundColor, "Appearance");
    RegisterField("backgroundAlpha", backgroundAlpha, "Appearance");
    RegisterField("guideValues", guideValues, "Guides");
    RegisterField("guideColor", guideColor, "Guides");
    RegisterField("guideAlpha", guideAlpha, "Guides");
}

void UILineGraph::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    sampleCount = std::clamp(sampleCount, 8, 2000);
    lineWidth = std::clamp(lineWidth, .5f, 8.f);
    backgroundAlpha = std::clamp(backgroundAlpha, 0.f, 1.f);
    guideAlpha = std::clamp(guideAlpha, 0.f, 1.f);
    ClearValues();
}

void UILineGraph::SetSeries(size_t index, const std::string& name, const glm::vec3& color)
{
    std::lock_guard lock(m_mutex);
    if (m_series.size() <= index) m_series.resize(index + 1);
    m_series[index].name = name;
    m_series[index].color = color;
}

void UILineGraph::Push(size_t index, float value)
{
    std::lock_guard lock(m_mutex);
    const size_t count = static_cast<size_t>(std::clamp(sampleCount, 8, 2000));
    if (m_series.size() <= index) m_series.resize(index + 1);
    auto& values = m_series[index].values;
    if (values.size() != count) values.assign(count, std::nanf(""));
    values[m_head % count] = std::isfinite(value) ? value : std::nanf("");
}

void UILineGraph::Advance()
{
    std::lock_guard lock(m_mutex);
    const size_t count = static_cast<size_t>(std::clamp(sampleCount, 8, 2000));
    m_head = (m_head + 1) % count;
    m_filled = std::min(m_filled + 1, count);
    // Series not pushed this frame show a gap there.
    for (auto& series : m_series)
    {
        if (series.values.size() != count) series.values.assign(count, std::nanf(""));
        series.values[m_head] = std::nanf("");
    }
}

void UILineGraph::ClearValues()
{
    std::lock_guard lock(m_mutex);
    for (auto& series : m_series) series.values.clear();
    m_head = 0;
    m_filled = 0;
}

UILineGraph::View UILineGraph::Snapshot() const
{
    View view;
    std::stringstream guides(guideValues);
    for (std::string item; std::getline(guides, item, ',');)
    {
        try { view.guides.push_back(std::stof(item)); } catch (...) {}
    }
    std::lock_guard lock(m_mutex);
    const size_t count = static_cast<size_t>(std::clamp(sampleCount, 8, 2000));
    float largest = maximum;
    view.series.reserve(m_series.size());
    for (const auto& series : m_series)
    {
        Series copy{ series.name, series.color, {} };
        copy.values.resize(count, std::nanf(""));
        if (series.values.size() == count)
            // Oldest first: the slot after the head is the oldest.
            for (size_t i = 0; i < count; ++i)
            {
                copy.values[i] = series.values[(m_head + 1 + i) % count];
                if (autoRange && std::isfinite(copy.values[i])) largest = std::max(largest, copy.values[i]);
            }
        view.series.push_back(std::move(copy));
    }
    view.minimum = minimum;
    view.maximum = std::max(largest, minimum + 1e-3f);
    return view;
}
}
