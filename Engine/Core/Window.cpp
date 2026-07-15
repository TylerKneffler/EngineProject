#include "Window.h"

// ---------------------------------------------------------------------------
// Instance forwarding via GWLP_USERDATA
//
// Win32 requires WndProc to be a free/static function, but we want access to
// the Window instance from inside it. The trick:
//   1. Pass 'this' as the lpCreateParams argument to CreateWindowExW.
//   2. In WM_NCCREATE (the very first message, delivered synchronously during
//      CreateWindowExW before the call returns), cast lParam to CREATESTRUCTW*
//      and read lpCreateParams to get the Window*.
//   3. Store that pointer in GWLP_USERDATA — a 64-bit user-data slot that
//      Win32 reserves on every HWND for exactly this purpose.
//   4. On every subsequent message, retrieve the pointer with
//      GetWindowLongPtrW and cast back to Window*.
// ---------------------------------------------------------------------------

Window::Window(HINSTANCE hInstance, const wchar_t* title, uint32_t width, uint32_t height)
    : m_width(width), m_height(height)
{
    const wchar_t* className = L"EngineWindowClass";

    // ---- Register window class ----
    // A WNDCLASSEXW describes shared properties for all windows created from
    // this class. Key fields:
    //   cbSize        — must be sizeof(WNDCLASSEXW); Win32 uses it for
    //                   versioning (it won't accept a wrong size).
    //   style         — CS_HREDRAW | CS_VREDRAW force a full repaint when
    //                   the window is resized horizontally or vertically.
    //   lpfnWndProc   — pointer to our static WndProc callback.
    //   hInstance     — the EXE module that owns this class.
    //   hCursor       — default cursor shape (IDC_ARROW = standard pointer).
    //   lpszClassName — unique string that names the class; CreateWindowExW
    //                   references it by this name.
    //
    // RegisterClassExW only needs to be called once per class name per process.
    // In a real engine you would check the return value or guard against
    // double-registration, but for a single-window app this is fine.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    // ---- Compute client-area-accurate window size ----
    // WS_OVERLAPPEDWINDOW windows have a title bar, resize borders, and system
    // menu — all of which eat into the total window rectangle. AdjustWindowRect
    // inflates a desired client rect to the full window rect, so that after the
    // OS subtracts the non-client decoration, the remaining client area is
    // exactly width x height pixels.
    RECT rc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    // ---- Create the window ----
    // CreateWindowExW allocates the kernel HWND object, registers the class,
    // and sends WM_NCCREATE / WM_CREATE to WndProc before returning. Passing
    // 'this' as the last argument (lpCreateParams) lets WndProc retrieve the
    // Window* during WM_NCCREATE via CREATESTRUCTW::lpCreateParams.
    //
    // CW_USEDEFAULT lets Windows choose the initial position automatically,
    // tiling new windows across the screen in a staircase pattern.
    m_hwnd = CreateWindowExW(
        0, className, title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, this   // pass 'this' as lpCreateParams
    );

    if (!m_hwnd)
        throw std::runtime_error("Failed to create window.");

    // Note: We do NOT call ShowWindow here. The window will be shown after
    // the caller has wired up all callbacks (OnUpdate, OnResize, WndProcHook).
    // This prevents Windows from sending messages before the callbacks are ready.
}

Window::~Window()
{
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}

void Window::Show()
{
    if (!m_hwnd)
        return;

    // SW_SHOWDEFAULT honours any show-state flags passed on the command line
    // (e.g. minimised or maximised start). UpdateWindow sends WM_PAINT
    // immediately so the window appears painted before the first frame runs.
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
}

int Window::Run()
{
    OutputDebugStringA("[Window::Run] Entering message loop\n");
    // ---- Game-style message loop ----
    // A traditional Win32 loop uses GetMessage(), which blocks the thread when
    // the queue is empty. That is fine for event-driven apps (text editors,
    // dialogs) but wrong for a renderer that must produce frames continuously.
    //
    // PeekMessage(PM_REMOVE) is non-blocking: it dequeues and removes one
    // message if one is available, or returns FALSE immediately if the queue
    // is empty. We drain the entire queue in an inner loop, then fall through
    // to OnUpdate() for the frame work.
    //
    // WM_QUIT is special — it is never sent to WndProc. GetMessage/PeekMessage
    // return it directly. We check for it here and exit the loop, returning
    // the embedded exit code (wParam) to the caller (typically WinMain).
    //
    // TranslateMessage   — converts WM_KEYDOWN + VK_* into WM_CHAR messages
    //                      for text input. Harmless to call for non-key msgs.
    // DispatchMessageW   — routes the message to WndProc for the target HWND.
    MSG msg{};
    while (true)
    {
        // Drain all pending messages without blocking.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return static_cast<int>(msg.wParam);

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // ---- Deferred resize ----
        // WM_SIZE is processed above inside DispatchMessageW, which writes
        // m_pendingWidth/Height and sets m_resizePending. We apply the resize
        // here, after the queue is empty and outside any WndProc call stack,
        // so there is no risk of re-entrant Present() / FlushGPU() calls.
        if (m_resizePending)
        {
            m_resizePending = false;
            m_width  = m_pendingWidth;
            m_height = m_pendingHeight;
            if (OnResize)
            {
                OnResize(m_width, m_height);
            }
        }

        // No pending messages — run one frame.
        if (OnUpdate)
        {
            OnUpdate();
        }
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ---- Instance pointer setup (WM_NCCREATE) ----
    // WM_NCCREATE is the first message dispatched to a new window, sent
    // synchronously during CreateWindowExW before that call returns. lParam
    // points to a CREATESTRUCTW whose lpCreateParams field holds whatever was
    // passed as the last argument to CreateWindowExW — our Window*.
    //
    // We store it in GWLP_USERDATA immediately so it is available for every
    // subsequent message, including WM_CREATE which follows right after.
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    // Retrieve the instance pointer. May be nullptr for very early messages
    // (before WM_NCCREATE), but those fall through to DefWindowProcW anyway.
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    
    if (self && self->WndProcHook)
        if (self->WndProcHook(hwnd, msg, wParam, lParam))
            return true;

    switch (msg)
    {

        case WM_SIZE:
            // ---- Deferred resize ----
            // Record the new client dimensions; Run() will apply the resize between
            // frames. We skip SIZE_MINIMIZED because the client area is 0x0 when
            // minimised — passing that to the renderer would create zero-sized
            // swap-chain buffers, which is invalid.
            //
            // LOWORD(lParam) = new client width,  HIWORD(lParam) = new client height.
            if (self && wParam != SIZE_MINIMIZED)
            {
                self->m_pendingWidth  = LOWORD(lParam);
                self->m_pendingHeight = HIWORD(lParam);
                self->m_resizePending = true;
            }
            return 0;

        case WM_DESTROY:
            // WM_DESTROY is sent when the window is being destroyed (after the user
            // clicks the X button or DestroyWindow is called). PostQuitMessage(0)
            // places WM_QUIT in the thread message queue with wParam = 0. Run()
            // detects that and returns, ending the application cleanly.
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            // VK_ESCAPE provides a quick keyboard exit for development builds.
            // wParam holds the virtual-key code; lParam holds repeat / scan info
            // (not needed here).
            if (wParam == VK_ESCAPE)
            {
                PostQuitMessage(0);
                return 0;
            }
            break;
        
    }

    // DefWindowProcW provides default handling for all messages we do not
    // process (e.g. WM_PAINT, WM_NCHITTEST, WM_SETCURSOR, …). Always call it
    // for unhandled messages or the window will behave incorrectly (no
    // resizing, no system menu, broken hit-testing, etc.).
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
