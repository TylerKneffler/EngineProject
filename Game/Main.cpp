#include "../Engine/Core/Window.h"
#include "../Engine/Renderers/Game/DX12GameRenderer.h"

// ---------------------------------------------------------------------------
// WinMain – entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       /*nShowCmd*/)
{
    // Create window (1280x720, windowed)
    auto window = std::make_unique<Window>(hInstance, L"DX12 Engine", 1280, 720);

    // Create renderer and initialise it against the window handle
    auto renderer = std::make_unique<DX12GameRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    // Wire resize callback so the swap chain stays in sync
    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);
    };

    // -----------------------------------------------------------------------
    // Main loop callback: clear to a dark-blue/teal colour each frame.
    // This is where you would later call your game-update + draw calls.
    // -----------------------------------------------------------------------
    window->OnUpdate = [&]()
    {
        renderer->BeginFrame();
        renderer->Clear(1.0f, 1.0f, 1.0f);   // RGBA – white
        renderer->EndFrame();
    };

    return window->Run();
}
