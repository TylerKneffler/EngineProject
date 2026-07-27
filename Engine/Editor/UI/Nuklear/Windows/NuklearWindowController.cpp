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

#include "Engine/Editor/UI/Nuklear/Windows/NuklearWindowController.h"

void NuklearWindowController::SetNextRect(
    float x, float y, float width, float height)
{
    m_x = x;
    m_y = y;
    m_width = width;
    m_height = height;
    m_hasNextRect = true;
}

bool NuklearWindowController::Begin(const char* title, bool* open)
{
    m_begun = false;
    m_currentWindow.clear();
    if (!m_context || (open && !*open)) return false;

    const auto closed = m_closedWindows.find(title);
    if (closed != m_closedWindows.end())
    {
        nk_window_show(m_context, title, NK_SHOWN);
        m_closedWindows.erase(closed);
    }

    const struct nk_rect bounds =
        nk_rect(m_x, m_y, m_width, m_height);
    if (m_hasNextRect)
        nk_window_set_bounds(m_context, title, bounds);
    m_hasNextRect = false;

    nk_flags flags =
        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE;
    if (open) flags |= NK_WINDOW_CLOSABLE;
    const bool visible = nk_begin(m_context, title, bounds, flags) != 0;
    m_begun = true;
    m_currentWindow = title;

    // Nuklear's title-bar X hides rather than destroys its window.
    if (open && nk_window_is_hidden(m_context, title))
    {
        *open = false;
        m_closedWindows.emplace(title);
    }
    return visible && (!open || *open);
}

void NuklearWindowController::End()
{
    if (!m_begun || !m_context) return;
    nk_end(m_context);
    m_begun = false;
    m_currentWindow.clear();
}
