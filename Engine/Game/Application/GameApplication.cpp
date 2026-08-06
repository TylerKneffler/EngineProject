#include "Game/Application/GameApplication.h"

#include "Game/Startup/GameProjectStartup.h"
#include "Game/Startup/RendererStartup.h"
#include "Core/Window.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/Rotate.h"
#include "Core/Serialization/SceneSerializer.h"
#endif

namespace Engine::Game
{
namespace
{
void RegisterGameComponents()
{
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
    RegisterComponentType<Rotate>("Rotate");
#endif
}

std::unique_ptr<IGameRenderer> CreateRenderer(
    const ProjectSettings& settings, Window& window)
{
    try
    {
        std::unique_ptr<IGameRenderer> renderer =
            RendererFactory::CreateGameRenderer(settings);
        if (!renderer)
            throw std::runtime_error("The renderer factory returned no renderer.");
        if (!renderer->Init(window.GetHWND(), window.GetWidth(), window.GetHeight()))
            throw std::runtime_error("Renderer initialization returned false.");
        return renderer;
    }
    catch (const std::exception& error)
    {
        const std::string message = "Failed to initialize the " +
            settings.gameRenderingAPI + " renderer.\n\n"
            "Please ensure it is installed, supported by your GPU, and that "
            "your graphics driver is current.\n\nDetails: " + error.what();
        MessageBoxA(window.GetHWND(), message.c_str(),
            "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return nullptr;
    }
}
}

int GameApplication::Run(HINSTANCE instance)
{
    RegisterGameComponents();
    ProjectSettings settings = LoadGameProjectSettings();
    if (!SelectStartupRenderer(settings))
        return 0;

    auto window = std::make_unique<Window>(instance, L"Game",
        settings.viewportWidth, settings.viewportHeight);
    std::unique_ptr<IGameRenderer> renderer = CreateRenderer(settings, *window);
    if (!renderer)
        return 1;

    Scene scene;
    scene.SetEditorMode2D(
        settings.editorMode == ProjectSettings::EditorMode::TwoD);
    scene.Init(renderer->GetGraphicsProvider());
    SceneManager::SetActiveScene(&scene);
    if (!scene.Load(settings.defaultScene))
        scene.Load(GetFallbackScenePath());

    for (const auto& object : scene.GetObjects())
        object->Start();

    LARGE_INTEGER performanceFrequency{};
    LARGE_INTEGER lastCounter{};
    QueryPerformanceFrequency(&performanceFrequency);
    QueryPerformanceCounter(&lastCounter);

    window->OnResize = [&renderer](uint32_t width, uint32_t height)
    {
        renderer->Resize(width, height);
    };
    window->OnUpdate = [&]()
    {
        LARGE_INTEGER currentCounter{};
        QueryPerformanceCounter(&currentCounter);
        [[maybe_unused]] const float deltaTime =
            static_cast<float>(currentCounter.QuadPart - lastCounter.QuadPart) /
            static_cast<float>(performanceFrequency.QuadPart);
        lastCounter = currentCounter;

        for (const auto& object : scene.GetObjects())
            object->Update();

        renderer->BeginFrame();
        renderer->Clear(0.1f, 0.1f, 0.1f);
        const float aspect = static_cast<float>(window->GetWidth()) /
            static_cast<float>(window->GetHeight());
        std::unique_ptr<IGraphicsContext> graphicsContext =
            renderer->CreateFrameGraphicsContext();
        Camera* gameCamera = scene.FindGameCamera();
        if (graphicsContext && gameCamera)
            scene.Render(graphicsContext.get(), aspect, gameCamera, false);
        renderer->EndFrame();
    };

    window->Show();
    return window->Run();
}
}
