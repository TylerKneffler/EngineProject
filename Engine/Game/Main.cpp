#include "Core/Window.h"
#include "Core/Renderers/Game/DX12GameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Scripts/Rotate.h"

// Fallback for IntelliSense — CMake overrides this with the real absolute path.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif

// ---------------------------------------------------------------------------
// WinMain — Game entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_     LPWSTR    /*lpCmdLine*/,
    _In_     int       /*nShowCmd*/)
{
    auto window   = std::make_unique<Window>(hInstance, L"Game", 1280, 720);
    auto renderer = std::make_unique<DX12GameRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    ID3D12Device* device = renderer->GetDevice();

    Scene scene;
    scene.Init(device);

    // Load the default scene content.
    Object* cubeObj = scene.AddObject("Cube");
    Mesh*   mesh    = cubeObj->AddComponent<Mesh>();
    cubeObj->AddComponent<Material>();
    cubeObj->AddComponent<Rotate>(); // spins 45 deg/s around Y by default
    mesh->LoadFromFile(ASSETS_PATH "cube.obj");
    mesh->CreateBuffer(device);

    // Start lifecycle.
    for (const auto& obj : scene.GetObjects())
        obj->Start();

    // High-resolution timer for delta time.
    LARGE_INTEGER perfFreq, lastCounter;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastCounter);

    window->OnResize = [&](uint32_t w, uint32_t h)
    {
        renderer->Resize(w, h);
    };

    window->OnUpdate = [&]()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        [[maybe_unused]] float dt = static_cast<float>(now.QuadPart - lastCounter.QuadPart)
                                  / static_cast<float>(perfFreq.QuadPart);
        lastCounter = now;

        for (const auto& obj : scene.GetObjects())
            obj->Update();

        renderer->BeginFrame();
        renderer->Clear(0.1f, 0.1f, 0.1f);
        // TODO: game renderer scene draw pass
        renderer->EndFrame();
    };

    return window->Run();
}
