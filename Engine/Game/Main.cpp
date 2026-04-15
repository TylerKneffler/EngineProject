#include "Core/Window.h"
#include "Core/Renderers/Game/DX12GameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/ProjectLoader.h"
#include "Scripts/Rotate.h"

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif
#ifndef PROJECT_FILE
#define PROJECT_FILE "Example_Proj.proj"
#endif
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
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
    // Load project configuration.
    ProjectLoader projectLoader;
    ProjectSettings projectSettings;
    try
    {
        projectSettings = projectLoader.LoadProject(PROJECT_FILE);
    }
    catch (const std::exception&)
    {
        projectSettings.assetsDirectory = ASSETS_PATH;
        projectSettings.defaultScene    = std::string(ASSETS_PATH) + "Scenes/default.scene";
        projectSettings.viewportWidth   = 1280;
        projectSettings.viewportHeight  = 720;
    }

    auto window   = std::make_unique<Window>(hInstance, L"Game",
                        projectSettings.viewportWidth,
                        projectSettings.viewportHeight);
    auto renderer = std::make_unique<DX12GameRenderer>();
    renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight());

    ID3D12Device* device = renderer->GetDevice();

    Scene scene;
    scene.Init(device);

    // Register script types for scene deserialization.
    SceneSerializer::Register("Rotate", []() -> Component* { return new Rotate(); });

    // Load the project's entry scene; fall back to the engine's built-in default.
    if (!scene.Load(projectSettings.defaultScene))
        scene.Load(std::string(ENGINE_ASSETS_PATH) + "Scenes/default.scene");

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
