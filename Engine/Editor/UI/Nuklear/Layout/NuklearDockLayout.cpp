#include "pch.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_API extern "C"
#include "nuklear.h"

#include "Engine/Editor/UI/Nuklear/Layout/NuklearDockLayout.h"

#include <algorithm>

namespace
{
constexpr float kMinimumLeftWidth = 180.f;
constexpr float kMinimumCenterWidth = 240.f;
constexpr float kMinimumRightWidth = 220.f;
constexpr float kMinimumWorkspaceHeight = 180.f;
constexpr float kMinimumConsoleHeight = 120.f;

void DrawSplitter(nk_context& context, const char* name,
    const struct nk_rect& bounds, const char* grip)
{
    nk_window_set_bounds(&context, name, bounds);
    if (nk_begin(&context, name, bounds, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        nk_layout_row_dynamic(&context, std::max(1.f, bounds.h - 4.f), 1);
        nk_label(&context, grip, NK_TEXT_CENTERED);
    }
    nk_end(&context);
}
}

NuklearDockGeometry NuklearDockLayout::Calculate(float width, float height) const
{
    NuklearDockGeometry result;
    result.width = width;
    result.height = height;

    const float maximumLeft = std::max(kMinimumLeftWidth,
        width - kMinimumCenterWidth - kMinimumRightWidth - result.splitterSize * 2.f);
    result.left = std::clamp(width * m_leftFraction,
        std::min(kMinimumLeftWidth, maximumLeft), maximumLeft);

    const float maximumRight = std::max(kMinimumRightWidth,
        width - result.left - kMinimumCenterWidth - result.splitterSize * 2.f);
    result.right = std::clamp(width * m_rightFraction,
        std::min(kMinimumRightWidth, maximumRight), maximumRight);
    result.center = std::max(1.f,
        width - result.left - result.right - result.splitterSize * 2.f);

    const float usableHeight =
        std::max(1.f, height - result.toolbarHeight - result.splitterSize);
    const float maximumConsole = std::max(kMinimumConsoleHeight,
        usableHeight - kMinimumWorkspaceHeight);
    result.console = std::clamp(height * m_bottomFraction,
        std::min(kMinimumConsoleHeight, maximumConsole), maximumConsole);
    result.workspace = std::max(1.f, usableHeight - result.console);
    return result;
}

NuklearDockGeometry NuklearDockLayout::Update(
    nk_context& context, float width, float height)
{
    NuklearDockGeometry geometry = Calculate(width, height);
    const struct nk_mouse& mouse = context.input.mouse;
    const struct nk_rect leftSplitter = nk_rect(
        geometry.left, geometry.toolbarHeight,
        geometry.splitterSize, geometry.workspace);
    const struct nk_rect rightSplitter = nk_rect(
        geometry.RightX() - geometry.splitterSize, geometry.toolbarHeight,
        geometry.splitterSize, geometry.workspace);
    const struct nk_rect bottomSplitter = nk_rect(
        0.f, geometry.toolbarHeight + geometry.workspace,
        width, geometry.splitterSize);

    if (mouse.buttons[NK_BUTTON_LEFT].clicked)
    {
        if (nk_input_is_mouse_hovering_rect(&context.input, leftSplitter))
            m_draggedSplitter = 1;
        else if (nk_input_is_mouse_hovering_rect(&context.input, rightSplitter))
            m_draggedSplitter = 2;
        else if (nk_input_is_mouse_hovering_rect(&context.input, bottomSplitter))
            m_draggedSplitter = 3;
    }

    if (!mouse.buttons[NK_BUTTON_LEFT].down)
        m_draggedSplitter = 0;
    else if (m_draggedSplitter == 1)
        m_leftFraction = std::clamp(mouse.pos.x / std::max(1.f, width), .10f, .55f);
    else if (m_draggedSplitter == 2)
        m_rightFraction = std::clamp(
            (width - mouse.pos.x) / std::max(1.f, width), .10f, .55f);
    else if (m_draggedSplitter == 3)
        m_bottomFraction = std::clamp(
            (height - mouse.pos.y) / std::max(1.f, height), .12f, .55f);

    return Calculate(width, height);
}

void NuklearDockLayout::DrawSplitters(
    nk_context& context, const NuklearDockGeometry& geometry) const
{
    DrawSplitter(context, "Left Dock Splitter",
        nk_rect(geometry.left, geometry.toolbarHeight,
            geometry.splitterSize, geometry.workspace), "|");
    DrawSplitter(context, "Right Dock Splitter",
        nk_rect(geometry.RightX() - geometry.splitterSize, geometry.toolbarHeight,
            geometry.splitterSize, geometry.workspace), "|");
    DrawSplitter(context, "Bottom Dock Splitter",
        nk_rect(0.f, geometry.toolbarHeight + geometry.workspace,
            geometry.width, geometry.splitterSize), "-");
}
