#pragma once
#include "../pch.h"

// ---------------------------------------------------------------------------
// Window — Tutorial Overview
//
// Win32 is the native Windows application API. Every visible window is a
// kernel object identified by an HWND (handle to window). To create one you
// must first register a "window class" that describes shared properties
// (cursor, icon, background brush, the WndProc callback address), then call
// CreateWindowExW to instantiate a window from that class.
//
// ---- Message loop ----
// Windows communicates with your application by posting messages (WM_SIZE,
// WM_KEYDOWN, WM_DESTROY, …) to a per-thread message queue. Your application
// must drain that queue regularly or Windows will consider it "not responding".
// The classic approach is:
//
//   while (GetMessage(&msg, …))   // blocks until a message arrives
//   {
//       TranslateMessage(&msg);  // synthesises WM_CHAR from WM_KEYDOWN
//       DispatchMessage(&msg);   // routes the message to WndProc
//   }
//
// For a game/renderer we need the loop to spin continuously even when no
// messages are queued, so we use PeekMessage (non-blocking) instead and call
// OnUpdate() after every empty drain pass.
//
// ---- WndProc and instance forwarding ----
// WndProc must be a free function or static method because Win32 stores only
// a raw function pointer. We work around this by storing the Window* in the
// GWLP_USERDATA slot of the HWND (a per-window user-data field). WM_NCCREATE
// is the first message the system sends during CreateWindowExW, so we capture
// the Window* (passed as lpCreateParams) there — before any other messages
// could arrive — and retrieve it on every subsequent call via
// GetWindowLongPtrW. This gives WndProc access to non-static members.
//
// ---- Deferred resize ----
// WM_SIZE is delivered synchronously inside DispatchMessage. DXGI's Present()
// can also call DispatchMessage internally, so it is possible to receive
// WM_SIZE re-entrantly while the GPU command queue is mid-flight. Calling
// ResizeBuffers() from that re-entrant context causes DXGI_ERROR_INVALID_CALL.
// The safe pattern is to record the new size in WM_SIZE and apply it in the
// main loop between frames, where no GPU work is in flight.
//
// ---- Callback pattern ----
// Rather than subclassing Window, callers attach lambdas to OnUpdate and
// OnResize. This keeps the window layer decoupled from the renderer and game
// logic — Main.cpp wires them together without the Window knowing about D3D12.
// ---------------------------------------------------------------------------
class Window
{

public:
    // Creates and shows a Win32 window whose *client area* is exactly
    // width x height pixels. Throws std::runtime_error on failure.
    Window(HINSTANCE hInstance, const wchar_t* title, uint32_t width, uint32_t height);
    ~Window();

    // Enters the message loop. Blocks until WM_QUIT is received (e.g. the
    // user closes the window or Escape is pressed). Returns the wParam value
    // from WM_QUIT, which is conventionally used as the process exit code.
    int Run();

    // Per-frame callbacks wired up by the caller (typically Main.cpp).
    // OnUpdate  — called once per frame after the message queue is drained.
    //             Perform simulation, input handling, and rendering here.
    // OnResize  — called between frames when the window client area changes.
    //             Notify the renderer to resize its swap chain buffers.
    std::function<void()> OnUpdate;
    std::function<void(uint32_t w, uint32_t h)> OnResize;
    std::function<bool(HWND, UINT, WPARAM, LPARAM)> WndProcHook;

    HWND     GetHWND()   const { return m_hwnd; }
    uint32_t GetWidth()  const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    // Static WndProc required by Win32 (cannot be a non-static member).
    // Forwards to the Window instance stored in GWLP_USERDATA (see .cpp).
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND     m_hwnd   = nullptr;
    uint32_t m_width  = 0;   // current client-area width  (updated after deferred resize)
    uint32_t m_height = 0;   // current client-area height (updated after deferred resize)

    // Deferred-resize state.
    // WM_SIZE writes here; Run() applies the resize safely between frames.
    // See the class overview above for why direct resize from WndProc is unsafe.
    uint32_t m_pendingWidth  = 0;
    uint32_t m_pendingHeight = 0;
    bool     m_resizePending = false;

};
