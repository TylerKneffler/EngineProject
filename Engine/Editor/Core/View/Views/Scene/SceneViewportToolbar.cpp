#include "SceneViewportToolbar.h"

#include "Core/Scene/Scene.h"
#include <cmath>

namespace Engine::Editor
{
namespace
{
const char* RenderModeLabel(Engine::Model::SceneRenderMode mode)
{
    switch (mode)
    {
    case Engine::Model::SceneRenderMode::Unlit:
        return "Unlit";
    case Engine::Model::SceneRenderMode::Wireframe:
        return "Wire";
    default:
        return "Lit";
    }
}

bool Contains(EditorUiVec2 point, EditorUiVec2 center, float radius)
{
    const float x = point.x - center.x;
    const float y = point.y - center.y;
    return x * x + y * y <= radius * radius;
}

void DrawToolIcon(IEditorUi& ui, EditorTransformTool tool,
    EditorUiVec2 center, EditorUiColor color, float scale = 1.f)
{
    if (tool == EditorTransformTool::Translate)
    {
        ui.DrawViewportLine({center.x - 10.f * scale, center.y},
            {center.x + 10.f * scale, center.y}, color, 2.f * scale);
        ui.DrawViewportLine({center.x, center.y - 10.f * scale},
            {center.x, center.y + 10.f * scale}, color, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 12.f * scale, center.y},
            {center.x + 7.f * scale, center.y - 4.f * scale},
            {center.x + 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x - 12.f * scale, center.y},
            {center.x - 7.f * scale, center.y - 4.f * scale},
            {center.x - 7.f * scale, center.y + 4.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y - 12.f * scale},
            {center.x - 4.f * scale, center.y - 7.f * scale},
            {center.x + 4.f * scale, center.y - 7.f * scale}, color);
        ui.DrawViewportTriangle({center.x, center.y + 12.f * scale},
            {center.x - 4.f * scale, center.y + 7.f * scale},
            {center.x + 4.f * scale, center.y + 7.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Rotate)
    {
        ui.DrawViewportCircle(center, 10.f * scale, color, false, 2.f * scale);
        ui.DrawViewportTriangle({center.x + 11.f * scale, center.y - 2.f * scale},
            {center.x + 5.f * scale, center.y - 7.f * scale},
            {center.x + 12.f * scale, center.y - 9.f * scale}, color);
    }
    else if (tool == EditorTransformTool::Scale)
    {
        ui.DrawViewportLine({center.x - 8.f * scale, center.y + 8.f * scale},
            {center.x + 8.f * scale, center.y - 8.f * scale}, color, 3.f * scale);
        ui.DrawViewportCircle({center.x - 9.f * scale, center.y + 9.f * scale}, 4.f * scale,
            color, true);
        ui.DrawViewportCircle({center.x + 9.f * scale, center.y - 9.f * scale}, 4.f * scale,
            color, true);
    }
    else
    {
        ui.DrawViewportCircle({center.x, center.y + 4.f * scale}, 7.f * scale,
            color, false, 2.f * scale);
        for (int finger = -2; finger <= 2; ++finger)
            ui.DrawViewportLine({center.x + finger * 3.f * scale, center.y + 2.f * scale},
                {center.x + finger * 3.f * scale,
                    center.y + (-9.f + std::abs(finger)) * scale},
                color, 2.f * scale);
    }
}

void DrawGridIcon(IEditorUi& ui, EditorUiVec2 center,
    EditorUiColor color, float scale = 1.f)
{
    const float half = 9.f * scale;
    for (int line = -1; line <= 1; ++line)
    {
        const float offset = static_cast<float>(line) * 6.f * scale;
        ui.DrawViewportLine(
            { center.x - half, center.y + offset },
            { center.x + half, center.y + offset },
            color, 1.8f * scale);
        ui.DrawViewportLine(
            { center.x + offset, center.y - half },
            { center.x + offset, center.y + half },
            color, 1.8f * scale);
    }
}
}

bool SceneViewportToolbar::Draw(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    const bool toolbarConsumedClick = DrawTransformToolbar(ui, input);
    const bool gridToggleConsumedClick = DrawGridToggle(ui, input, scene);
    const bool renderModeConsumedClick =
        DrawRenderModeMenu(ui, input, scene);
    const bool sceneUiOverlayConsumedClick =
        DrawSceneUiOverlayToggle(ui, input, scene);
    return toolbarConsumedClick || gridToggleConsumedClick ||
        renderModeConsumedClick || sceneUiOverlayConsumedClick;
}

bool SceneViewportToolbar::DrawTransformToolbar(IEditorUi& ui,
    const EditorUiViewportInput& input)
{
    if (input.available.x < 28.f || input.available.y < 28.f)
        return false;

    constexpr float scale = 1.f / 3.f;
    constexpr float radius = 18.f * scale;
    constexpr float spacing = 44.f * scale;
    constexpr float padding = 8.f;
    const EditorUiVec2 mainCenter{padding + radius, padding + radius};
    bool consumed = false;
    if (input.rawLeftClicked && Contains(
        input.mousePosInViewport, mainCenter, radius))
    {
        m_transformToolbarExpanded = !m_transformToolbarExpanded;
        consumed = true;
    }

    const bool mainHovered = Contains(
        input.mousePosInViewport, mainCenter, radius);
    ui.DrawViewportCircle(mainCenter, radius,
        mainHovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                    : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(mainCenter, radius,
        {0.72f, 0.78f, 0.90f, 0.9f}, false, 1.5f * scale);
    DrawToolIcon(ui, m_transformTool, mainCenter,
        {0.92f, 0.95f, 1.f, 1.f}, scale);

    if (!m_transformToolbarExpanded)
        return consumed;

    constexpr EditorTransformTool tools[] = {
        EditorTransformTool::Translate,
        EditorTransformTool::Rotate,
        EditorTransformTool::Scale,
        EditorTransformTool::Hand
    };
    for (int index = 0; index < 4; ++index)
    {
        const EditorUiVec2 center{mainCenter.x, mainCenter.y +
            spacing * static_cast<float>(index + 1)};
        if (center.y + radius > input.available.y)
            break;
        const bool hovered = Contains(input.mousePosInViewport, center, radius);
        if (input.rawLeftClicked && hovered)
        {
            m_transformTool = tools[index];
            m_transformToolbarExpanded = false;
            consumed = true;
        }
        const bool selected = tools[index] == m_transformTool;
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                     : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                               : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
        ui.DrawViewportCircle(center, radius,
            selected ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                     : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
            1.5f * scale);
        DrawToolIcon(ui, tools[index], center,
            {0.92f, 0.95f, 1.f, 1.f}, scale);
    }
    return consumed;
}

bool SceneViewportToolbar::DrawGridToggle(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 44.f || input.available.y < 28.f)
        return false;

    constexpr float scale = 1.f / 3.f;
    constexpr float radius = 18.f * scale;
    constexpr float spacing = 44.f * scale;
    constexpr float padding = 8.f;
    const EditorUiVec2 center{padding + radius + spacing, padding + radius};
    if (center.x + radius > input.available.x)
        return false;

    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    bool consumed = false;
    if (input.rawLeftClicked && hovered)
    {
        scene->settings.showGrid = !scene->settings.showGrid;
        consumed = true;
    }

    const bool enabled = scene->settings.showGrid;
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                          : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    DrawGridIcon(ui, center, {0.92f, 0.95f, 1.f, 1.f}, scale);
    return consumed;
}

bool SceneViewportToolbar::DrawRenderModeMenu(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 60.f || input.available.y < 28.f)
        return false;

    constexpr float scale = 1.f / 3.f;
    constexpr float radius = 18.f * scale;
    constexpr float spacing = 44.f * scale;
    constexpr float padding = 8.f;
    const EditorUiVec2 center{
        padding + radius + spacing * 2.f,
        padding + radius
    };
    if (center.x + radius > input.available.x)
        return false;

    bool consumed = false;
    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    if (input.rawLeftClicked && hovered)
    {
        m_renderModeExpanded = !m_renderModeExpanded;
        consumed = true;
    }

    const Engine::Model::SceneRenderMode activeMode = scene->settings.renderMode;
    ui.DrawViewportCircle(center, radius,
        hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        {0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    ui.DrawViewportText({ center.x - 11.f * scale, center.y - 3.f * scale },
        RenderModeLabel(activeMode), {0.92f, 0.95f, 1.f, 1.f});

    if (!m_renderModeExpanded)
        return consumed;

    constexpr Engine::Model::SceneRenderMode modes[] = {
        Engine::Model::SceneRenderMode::Lit,
        Engine::Model::SceneRenderMode::Unlit,
        Engine::Model::SceneRenderMode::Wireframe
    };
    for (int index = 0; index < 3; ++index)
    {
        const EditorUiVec2 optionCenter{center.x,
            center.y + spacing * static_cast<float>(index + 1)};
        if (optionCenter.y + radius > input.available.y)
            break;
        const bool optionHovered =
            Contains(input.mousePosInViewport, optionCenter, radius);
        if (input.rawLeftClicked && optionHovered)
        {
            scene->settings.renderMode = modes[index];
            m_renderModeExpanded = false;
            consumed = true;
        }
        const bool selected = scene->settings.renderMode == modes[index];
        ui.DrawViewportCircle(optionCenter, radius,
            selected ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                     : optionHovered
                        ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                        : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
        ui.DrawViewportCircle(optionCenter, radius,
            selected ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                     : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
            1.5f * scale);
        ui.DrawViewportText(
            { optionCenter.x - 11.f * scale, optionCenter.y - 3.f * scale },
            RenderModeLabel(modes[index]), {0.92f, 0.95f, 1.f, 1.f});
    }

    return consumed;
}

bool SceneViewportToolbar::DrawSceneUiOverlayToggle(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene)
{
    if (!scene || input.available.x < 76.f || input.available.y < 28.f)
        return false;

    constexpr float scale = 1.f / 3.f;
    constexpr float radius = 18.f * scale;
    constexpr float spacing = 44.f * scale;
    constexpr float padding = 8.f;
    const EditorUiVec2 center{
        padding + radius + spacing * 3.f,
        padding + radius
    };
    if (center.x + radius > input.available.x)
        return false;

    const bool hovered = Contains(input.mousePosInViewport, center, radius);
    bool consumed = false;
    if (input.rawLeftClicked && hovered)
    {
        scene->settings.sceneViewUiOverlay =
            !scene->settings.sceneViewUiOverlay;
        consumed = true;
    }

    const bool enabled = scene->settings.sceneViewUiOverlay;
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.18f, 0.38f, 0.68f, 0.98f}
                : hovered ? EditorUiColor{0.22f, 0.25f, 0.31f, 0.98f}
                          : EditorUiColor{0.10f, 0.12f, 0.16f, 0.94f}, true);
    ui.DrawViewportCircle(center, radius,
        enabled ? EditorUiColor{0.45f, 0.72f, 1.f, 1.f}
                : EditorUiColor{0.62f, 0.68f, 0.78f, 0.9f}, false,
        1.5f * scale);
    ui.DrawViewportText(
        { center.x - 5.f * scale, center.y - 3.f * scale },
        "UI", {0.92f, 0.95f, 1.f, 1.f});
    return consumed;
}
}
