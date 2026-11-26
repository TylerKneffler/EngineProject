#pragma once

#include "Core/Component.h"
#include "Core/Components/PropertyMacros.h"
#include <glm/glm.hpp>
#include <mutex>
#include <string>
#include <vector>

namespace Engine::Components
{
// A live line graph inside the rect supplied by UIObject (UIRenderer draws
// it): a background, horizontal guide lines at fixed values, and one line per
// series over the last `sampleCount` values, oldest on the left. Series and
// their values are runtime data fed by a script (Push); the look and the
// value range are ordinary properties, editable in the inspector.
class UILineGraph final : public Engine::Core::Component
{
public:
    UILineGraph();

    // Values kept per series (the graph's width in samples).
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph", Range = "8, 2000")
    int sampleCount = 240;
    // Value range drawn bottom to top. With autoRange the top follows the
    // largest visible value (at least `maximum`), so spikes stay in view.
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    float minimum = 0.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    float maximum = 33.3f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    bool autoRange = true;
    // Line thickness in canvas units.
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph", Range = "0.5, 8")
    float lineWidth = 1.5f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    glm::vec3 backgroundColor { 0.02f, 0.025f, 0.03f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph", Range = "0, 1")
    float backgroundAlpha = 0.72f;
    // Horizontal guide lines at these values (comma separated), e.g. frame
    // times for 100 / 80 / 60 / 30 fps.
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    std::string guideValues = "10,12.5,16.67,33.3";
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph")
    glm::vec3 guideColor { 0.55f, 0.55f, 0.55f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Line graph", Range = "0, 1")
    float guideAlpha = 0.35f;

    struct Series
    {
        std::string name;
        glm::vec3 color { 1.f };
        std::vector<float> values;   // ring buffer, `sampleCount` long
    };
    // Runtime data (not serialized). Safe to call from the update thread
    // while the renderer reads a copy (Snapshot).
    void SetSeries(size_t index, const std::string& name, const glm::vec3& color);
    void Push(size_t index, float value);
    // Advances every series by one sample (call once per pushed frame).
    void Advance();
    void ClearValues();
    struct View
    {
        std::vector<Series> series; // values oldest first
        float minimum = 0.f, maximum = 1.f;
        std::vector<float> guides;
    };
    View Snapshot() const;

    void Deserialize(const JsonValue& value) override;

private:
    mutable std::mutex m_mutex;
    std::vector<Series> m_series;
    size_t m_head = 0;   // next slot to write
    size_t m_filled = 0; // samples written, up to sampleCount
};
}
