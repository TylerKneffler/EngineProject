#include "Game/Application/PerformanceOverlay.h"

#include "Core/Components/UI/Canvas.h"
#include "Core/Components/UI/UIImage.h"
#include "Core/Components/UI/UILineGraph.h"
#include "Core/Components/UI/UIObject.h"
#include "Core/Components/UI/UIText.h"
#include "Core/Scene/Object.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace Engine::Game
{
namespace
{
// Layout in canvas units (1080 high, y down: anchor y 0 is the top).
constexpr float PanelWidth = 720.f, Margin = 16.f, Pad = 16.f;
constexpr float HeaderHeight = 50.f, GraphHeight = 170.f, LegendHeight = 32.f;
constexpr float CellLabelHeight = 24.f, CellValueHeight = 32.f, RowGap = 10.f;
constexpr int CellsPerRow = 6;
constexpr float PanelHeight = Pad + HeaderHeight + 6.f + GraphHeight + 6.f + LegendHeight + RowGap +
    2.f * (CellLabelHeight + CellValueHeight) + RowGap + Pad;
constexpr size_t GraphSamples = 300;

const glm::vec3 White { 0.96f, 0.96f, 0.96f };
const glm::vec3 Grey { 0.75f, 0.77f, 0.8f };
struct SeriesStyle { const char* name; glm::vec3 color; };
// The graph's series: frame, GPU, main-thread CPU, update.
const SeriesStyle Series[] = {
    { "FRAME", { 1.f, 1.f, 1.f } },
    { "GPU", { 0.35f, 0.92f, 0.42f } },
    { "CPU MAIN", { 1.f, 0.64f, 0.22f } },
    { "UPDATE", { 0.38f, 0.76f, 1.f } },
};
// Metric cells: two rows of six, label above value.
const char* const CellLabels[2][CellsPerRow] = {
    { "GPU TERRAIN", "GRASS", "CULL", "OPAQUE", "SHADOW", "SKY+CLOUD" },
    { "CPU PREPARE", "CAMERA REC", "SHADOW REC", "RENDER THR", "> 33 MS", "> 50 MS" },
};

// Value text slots in m_valueIds.
enum Slot : size_t { FpsSlot, FrameSlot, LegendSlot0, CellSlot0 = LegendSlot0 + 4, SlotCount = CellSlot0 + 2 * CellsPerRow };

Engine::Core::Object* AddChild(Engine::Scene::Scene& scene, Engine::Core::Object* parent, const char* name)
{
    auto* object = scene.AddObject(name);
    object->runtimeOnly = true;
    if (parent) scene.SetParentLocal(object, parent);
    return object;
}

// A rect at (x, y) from its parent's top left, of the given size.
Engine::Components::UIObject* TopLeft(Engine::Core::Object* object, float x, float y, float width, float height, int zOrder)
{
    auto* rect = object->AddComponent<Engine::Components::UIObject>();
    rect->anchorMin = rect->anchorMax = { 0.f, 0.f, 0.f };
    rect->pivot = { 0.f, 0.f, 0.f };
    rect->anchoredPosition = { x, y, 0.f };
    rect->sizeDelta = { width, height, 0.f };
    rect->zOrder = zOrder;
    return rect;
}

Engine::Components::UIText* Text(Engine::Core::Object* object, float size, glm::vec3 color, const char* alignment = "Left")
{
    auto* text = object->AddComponent<Engine::Components::UIText>();
    text->fontSize = size;
    text->color = color;
    text->horizontalAlignment = alignment;
    text->verticalAlignment = "Center";
    text->overflow = "Clip";
    text->text.clear();
    return text;
}

double Percentile(std::vector<double> values, double share)
{
    if (values.empty()) return 0.0;
    const size_t index = std::min(values.size() - 1, size_t(share * double(values.size() - 1) + .5));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

std::string Number(const char* pattern, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}
}

void PerformanceOverlay::Push(const Sample& sample)
{
    if (!m_enabled.load()) return;
    std::lock_guard lock(m_mutex);
    if (m_pending.size() < 64) m_pending.push_back(sample);
}

void PerformanceOverlay::Update(Engine::Scene::Scene& scene)
{
    using namespace Engine::Components;
    auto* root = m_rootId ? scene.FindObjectById(m_rootId) : nullptr;
    if (!m_enabled.load())
    {
        if (root && root->enabled) root->enabled = false;
        std::lock_guard lock(m_mutex);
        m_pending.clear();
        return;
    }
    if (!root)
    {
        m_valueIds.assign(SlotCount, 0);
        root = scene.AddObject("Performance overlay");
        root->runtimeOnly = true;
        auto* canvas = root->AddComponent<Canvas>();
        canvas->referenceResolution = { 1920.f, 1080.f, 0.f };
        canvas->scaleMode = "ScaleWithScreen";
        canvas->matchWidthOrHeight = 1.f;
        canvas->sortingOrder = 1001;
        auto* rootRect = root->AddComponent<UIObject>();
        rootRect->anchorMin = { 0.f, 0.f, 0.f };
        rootRect->anchorMax = { 1.f, 1.f, 0.f };
        rootRect->pivot = { 0.f, 0.f, 0.f };
        rootRect->sizeDelta = { 0.f, 0.f, 0.f };

        // Panel: top right (anchor x 1, y 0), near-opaque black.
        auto* panel = AddChild(scene, root, "Performance panel");
        auto* panelRect = panel->AddComponent<UIObject>();
        panelRect->anchorMin = panelRect->anchorMax = { 1.f, 0.f, 0.f };
        panelRect->pivot = { 1.f, 0.f, 0.f };
        panelRect->anchoredPosition = { -Margin, Margin + m_topOffset, 0.f };
        panelRect->sizeDelta = { PanelWidth, PanelHeight, 0.f };
        panelRect->zOrder = 20;
        auto* background = panel->AddComponent<UIImage>();
        background->color = { 0.f, 0.f, 0.f };
        background->alpha = 1.f;

        const float inner = PanelWidth - 2.f * Pad;
        float y = Pad;
        // Header: big fps on the left, frame time and p99 on the right.
        auto* fps = AddChild(scene, panel, "Performance fps");
        TopLeft(fps, Pad, y, 220.f, HeaderHeight, 21);
        Text(fps, 40.f, White);
        m_valueIds[FpsSlot] = scene.GetObjectId(fps);
        auto* frame = AddChild(scene, panel, "Performance frame");
        TopLeft(frame, Pad + 220.f, y, inner - 220.f, HeaderHeight, 21);
        Text(frame, 26.f, White, "Right");
        m_valueIds[FrameSlot] = scene.GetObjectId(frame);
        y += HeaderHeight + 6.f;

        auto* graphObject = AddChild(scene, panel, "Performance graph");
        TopLeft(graphObject, Pad, y, inner, GraphHeight, 21);
        auto* graph = graphObject->AddComponent<UILineGraph>();
        graph->sampleCount = int(GraphSamples);
        graph->minimum = 0.f;
        // Fixed 0-25 ms: spikes clip at the top instead of flattening the
        // lines, and the 100 / 80 / 60 fps guides stay readable.
        graph->maximum = 25.f;
        graph->autoRange = false;
        graph->lineWidth = 2.f;
        graph->backgroundColor = { 0.f, 0.f, 0.f };
        graph->backgroundAlpha = 1.f;
        graph->guideValues = "10,12.5,16.67";
        graph->guideColor = { 0.7f, 0.7f, 0.7f };
        graph->guideAlpha = 0.3f;
        for (size_t i = 0; i < std::size(Series); ++i)
            graph->SetSeries(i, Series[i].name, Series[i].color);
        m_graphId = scene.GetObjectId(graphObject);
        y += GraphHeight + 6.f;

        // Legend: each series' name and current value in its colour.
        const float legendWidth = inner / float(std::size(Series));
        for (size_t i = 0; i < std::size(Series); ++i)
        {
            auto* legend = AddChild(scene, panel, "Performance legend");
            TopLeft(legend, Pad + legendWidth * float(i), y, legendWidth, LegendHeight, 21);
            Text(legend, 22.f, Series[i].color);
            m_valueIds[LegendSlot0 + i] = scene.GetObjectId(legend);
        }
        y += LegendHeight + RowGap;

        // Metric cells: small grey label over a larger value.
        const float cellWidth = inner / float(CellsPerRow);
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < CellsPerRow; ++column)
            {
                const float x = Pad + cellWidth * float(column);
                auto* label = AddChild(scene, panel, "Performance cell label");
                TopLeft(label, x, y, cellWidth, CellLabelHeight, 21);
                Text(label, 18.f, Grey)->text = CellLabels[row][column];
                auto* value = AddChild(scene, panel, "Performance cell value");
                TopLeft(value, x, y + CellLabelHeight, cellWidth, CellValueHeight, 21);
                Text(value, 26.f, row == 0 ? Series[1].color : Series[2].color);
                m_valueIds[CellSlot0 + size_t(row * CellsPerRow + column)] = scene.GetObjectId(value);
            }
            y += CellLabelHeight + CellValueHeight + RowGap;
        }
        m_rootId = scene.GetObjectId(root);
        m_textAgeMs = 1.e9;
    }
    if (!root->enabled) root->enabled = true;

    std::vector<Sample> samples;
    {
        std::lock_guard lock(m_mutex);
        samples.swap(m_pending);
    }
    auto* graphObject = scene.FindObjectById(m_graphId);
    auto* graph = graphObject ? graphObject->GetComponent<UILineGraph>() : nullptr;
    for (const auto& sample : samples)
    {
        const double mainMs = sample.updateMs + sample.prepareMs + sample.cameraRecordMs + sample.shadowRecordMs;
        if (graph)
        {
            graph->Push(0, float(sample.frameMs));
            if (sample.gpuValid) graph->Push(1, float(sample.gpuMs));
            graph->Push(2, float(mainMs));
            graph->Push(3, float(sample.updateMs));
            graph->Advance();
        }
        m_window.push_back(sample);
        if (m_window.size() > GraphSamples) m_window.pop_front();
        m_textAgeMs += sample.frameMs;
    }

    // Values four times a second (text layout is not free).
    if (m_window.empty() || m_textAgeMs < 250.0) return;
    m_textAgeMs = 0.0;
    // Averages over the last second; p99 and spikes over the graph's span.
    double frameSum = 0, gpu = 0, terrain = 0, grass = 0, cull = 0, opaque = 0, shadowGpu = 0, sky = 0,
        update = 0, prepare = 0, camera = 0, shadow = 0, renderThread = 0;
    size_t count = 0, gpuCount = 0;
    for (auto it = m_window.rbegin(); it != m_window.rend() && frameSum < 1000.0; ++it, ++count)
    {
        frameSum += it->frameMs;
        update += it->updateMs; prepare += it->prepareMs; camera += it->cameraRecordMs;
        shadow += it->shadowRecordMs; renderThread += it->renderThreadMs;
        if (it->gpuValid)
        {
            ++gpuCount; gpu += it->gpuMs; terrain += it->gpuTerrainMs; grass += it->gpuGrassMs;
            cull += it->gpuCullMs; opaque += it->gpuOpaqueMs; shadowGpu += it->gpuShadowMs; sky += it->gpuSkyCloudsMs;
        }
    }
    const double n = double(std::max<size_t>(count, 1)), g = double(std::max<size_t>(gpuCount, 1));
    std::vector<double> frames;
    frames.reserve(m_window.size());
    size_t over33 = 0, over50 = 0;
    for (const auto& sample : m_window)
    {
        frames.push_back(sample.frameMs);
        over33 += sample.frameMs > 33.3 ? 1 : 0;
        over50 += sample.frameMs > 50.0 ? 1 : 0;
    }
    const double frameMs = frameSum / n;
    const double mainMs = (update + prepare + camera + shadow) / n;
    std::vector<std::string> values(SlotCount);
    values[FpsSlot] = Number("%.0f FPS", frameMs > 0 ? 1000.0 / frameMs : 0.0);
    values[FrameSlot] = Number("%.1f MS", frameMs) + Number("   P99 %.1f MS", Percentile(frames, .99));
    values[LegendSlot0 + 0] = Number("FRAME %.1f", frameMs);
    values[LegendSlot0 + 1] = Number("GPU %.1f", gpu / g);
    values[LegendSlot0 + 2] = Number("CPU %.1f", mainMs);
    values[LegendSlot0 + 3] = Number("UPDATE %.1f", update / n);
    const double cells[2][CellsPerRow] = {
        { terrain / g, grass / g, cull / g, opaque / g, shadowGpu / g, sky / g },
        { prepare / n, camera / n, shadow / n, renderThread / n, double(over33), double(over50) },
    };
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < CellsPerRow; ++column)
            values[CellSlot0 + size_t(row * CellsPerRow + column)] =
                Number(row == 1 && column >= 4 ? "%.0f" : "%.1f", cells[row][column]);
    for (size_t slot = 0; slot < SlotCount; ++slot)
    {
        auto* object = m_valueIds[slot] ? scene.FindObjectById(m_valueIds[slot]) : nullptr;
        auto* text = object ? object->GetComponent<UIText>() : nullptr;
        if (text && text->text != values[slot]) text->text = std::move(values[slot]);
    }
}
}
