#include "Core/Window.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/ProjectLoader.h"
#include <filesystem>

// Fallback for IntelliSense — CMake overrides these with real absolute paths.
#ifndef ASSETS_PATH
#define ASSETS_PATH "Assets/"
#endif
#ifndef PROJECT_FILE
#define PROJECT_FILE ""
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
    std::string projectFile = PROJECT_FILE;
    std::vector<std::string> localProjects;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path()))
        if (entry.is_regular_file() && entry.path().extension() == ".proj")
            localProjects.push_back(entry.path().string());
    if (localProjects.size() == 1)
        projectFile = localProjects.front();
    try
    {
        projectSettings = projectLoader.LoadProject(projectFile);
    }
    catch (const std::exception&)
    {
        projectSettings.assetsDirectory = ENGINE_ASSETS_PATH;
        projectSettings.defaultScene    = std::string(ENGINE_ASSETS_PATH) + "Scenes/default.scene";
        projectSettings.viewportWidth   = 1280;
        projectSettings.viewportHeight  = 720;
        projectSettings.renderingAPI = "DirectX11";
        projectSettings.editorRenderingAPI = "DirectX11";
        projectSettings.gameRenderingAPI = "DirectX11";
        projectSettings.clearColor = { 0.1f, 0.1f, 0.1f, 1.f };
    }

    auto window   = std::make_unique<Window>(hInstance, L"Game",
                        projectSettings.viewportWidth,
                        projectSettings.viewportHeight);

    std::unique_ptr<IGameRenderer> renderer;
    try
    {
        renderer = RendererFactory::CreateGameRenderer(projectSettings);
    }
    catch (const std::exception& e)
    {
        std::string api = projectSettings.gameRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.\n\n"
            "Details: " + e.what();
        MessageBoxA(window->GetHWND(), msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!renderer)
    {
        std::string api = projectSettings.gameRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.";
        MessageBoxA(window->GetHWND(), msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!renderer->Init(window->GetHWND(), window->GetWidth(), window->GetHeight()))
    {
        std::string api = projectSettings.gameRenderingAPI;
        std::string msg = "Failed to initialize the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU driver is up to date.";
        MessageBoxA(window->GetHWND(), msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    Scene scene;
    scene.Init(renderer->GetGraphicsProvider());
    SceneManager::SetActiveScene(&scene);

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
        auto graphicsContext = renderer->CreateFrameGraphicsContext();
        Camera* gameCamera = scene.FindGameCamera();
        if (graphicsContext)
            scene.Render(graphicsContext.get(), aspect, gameCamera);
        
        renderer->EndFrame();
    };

    // Show the window after all callbacks are set up
    window->Show();

    return window->Run();
}
