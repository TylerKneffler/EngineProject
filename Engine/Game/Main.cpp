#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/DX12/DX12GraphicsContext.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
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
    auto renderer = RendererFactory::CreateGameRenderer(projectSettings);
    if (!renderer || !renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight()))
        return 1;

    // For D3D12-specific operations, cast to concrete renderer
    auto* dx12Renderer = dynamic_cast<DX12GameRenderer*>(renderer.get());
    if (!dx12Renderer)
        return 1;

    Scene scene;
    scene.Init(dx12Renderer->GetGraphicsProvider());
    SceneManager::SetActiveScene(&scene);

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
        
        // Render the scene
        float aspect = static_cast<float>(window->GetWidth()) / static_cast<float>(window->GetHeight());
        D3D12GraphicsContext graphicsContext(dx12Renderer->GetCommandList());
        Camera* gameCamera = scene.FindGameCamera();
        scene.Render(&graphicsContext, aspect, gameCamera);
        
        renderer->EndFrame();
    };

    return window->Run();
}
