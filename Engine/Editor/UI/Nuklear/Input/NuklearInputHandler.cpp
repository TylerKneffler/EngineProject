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

#include "Engine/Editor/UI/Nuklear/Input/NuklearInputHandler.h"

namespace
{
int MouseX(intptr_t lParam) { return static_cast<short>(LOWORD(lParam)); }
int MouseY(intptr_t lParam) { return static_cast<short>(HIWORD(lParam)); }

void ReleaseMouseButtons(nk_context& context)
{
    const int x = static_cast<int>(context.input.mouse.pos.x);
    const int y = static_cast<int>(context.input.mouse.pos.y);
    nk_input_button(&context, NK_BUTTON_LEFT, x, y, 0);
    nk_input_button(&context, NK_BUTTON_DOUBLE, x, y, 0);
    nk_input_button(&context, NK_BUTTON_RIGHT, x, y, 0);
    nk_input_button(&context, NK_BUTTON_MIDDLE, x, y, 0);
}
}

bool NuklearInputHandler::HandleMessage(nk_context& context, void* nativeWindow,
    uint32_t message, uintptr_t wParam, intptr_t lParam) const
{
    const int x = MouseX(lParam);
    const int y = MouseY(lParam);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
    {
        switch (wParam)
        {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            nk_input_key(&context, NK_KEY_SHIFT, down); return true;
        case VK_DELETE: nk_input_key(&context, NK_KEY_DEL, down); return true;
        case VK_RETURN: nk_input_key(&context, NK_KEY_ENTER, down); return true;
        case VK_TAB: nk_input_key(&context, NK_KEY_TAB, down); return true;
        case VK_UP: nk_input_key(&context, NK_KEY_UP, down); return true;
        case VK_DOWN: nk_input_key(&context, NK_KEY_DOWN, down); return true;
        case VK_LEFT:
            nk_input_key(&context, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down);
            return true;
        case VK_RIGHT:
            nk_input_key(&context, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down);
            return true;
        case VK_BACK: nk_input_key(&context, NK_KEY_BACKSPACE, down); return true;
        case VK_HOME:
            nk_input_key(&context, NK_KEY_TEXT_START, down);
            nk_input_key(&context, NK_KEY_SCROLL_START, down);
            return true;
        case VK_END:
            nk_input_key(&context, NK_KEY_TEXT_END, down);
            nk_input_key(&context, NK_KEY_SCROLL_END, down);
            return true;
        case VK_NEXT: nk_input_key(&context, NK_KEY_SCROLL_DOWN, down); return true;
        case VK_PRIOR: nk_input_key(&context, NK_KEY_SCROLL_UP, down); return true;
        case 'A':
            if (ctrl) { nk_input_key(&context, NK_KEY_TEXT_SELECT_ALL, down); return true; }
            break;
        case 'C':
            if (ctrl) { nk_input_key(&context, NK_KEY_COPY, down); return true; }
            break;
        case 'V':
            if (ctrl) { nk_input_key(&context, NK_KEY_PASTE, down); return true; }
            break;
        case 'X':
            if (ctrl) { nk_input_key(&context, NK_KEY_CUT, down); return true; }
            break;
        case 'Z':
            if (ctrl) { nk_input_key(&context, NK_KEY_TEXT_UNDO, down); return true; }
            break;
        case 'R':
            if (ctrl) { nk_input_key(&context, NK_KEY_TEXT_REDO, down); return true; }
            break;
        }
        return false;
    }

    switch (message)
    {
    case WM_CHAR:
        if (wParam >= 32)
        {
            nk_input_unicode(&context, static_cast<nk_rune>(wParam));
            return true;
        }
        break;
    case WM_LBUTTONDOWN:
        nk_input_button(&context, NK_BUTTON_LEFT, x, y, 1);
        SetCapture(static_cast<HWND>(nativeWindow));
        return true;
    case WM_LBUTTONUP:
        nk_input_button(&context, NK_BUTTON_LEFT, x, y, 0);
        nk_input_button(&context, NK_BUTTON_DOUBLE, x, y, 0);
        ReleaseCapture();
        return true;
    case WM_LBUTTONDBLCLK:
        nk_input_button(&context, NK_BUTTON_DOUBLE, x, y, 1);
        return true;
    case WM_RBUTTONDOWN:
        nk_input_button(&context, NK_BUTTON_RIGHT, x, y, 1);
        SetCapture(static_cast<HWND>(nativeWindow));
        return true;
    case WM_RBUTTONUP:
        nk_input_button(&context, NK_BUTTON_RIGHT, x, y, 0);
        ReleaseCapture();
        return true;
    case WM_MBUTTONDOWN:
        nk_input_button(&context, NK_BUTTON_MIDDLE, x, y, 1);
        SetCapture(static_cast<HWND>(nativeWindow));
        return true;
    case WM_MBUTTONUP:
        nk_input_button(&context, NK_BUTTON_MIDDLE, x, y, 0);
        ReleaseCapture();
        return true;
    case WM_MOUSEMOVE:
        nk_input_motion(&context, x, y);
        return true;
    case WM_MOUSEWHEEL:
        nk_input_scroll(&context, nk_vec2(0.f,
            static_cast<float>(static_cast<short>(HIWORD(wParam))) / WHEEL_DELTA));
        return true;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        ReleaseMouseButtons(context);
        return false;
    }
    return false;
}
