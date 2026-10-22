#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void SaveFluidImage(const std::vector<uint8_t>& rgba, uint32_t width,
    uint32_t height)
{
    const auto path = std::filesystem::path(__FILE__).parent_path()
        .parent_path() / "artifacts" / "fluid-water-dx11.bmp";
    std::filesystem::create_directories(path.parent_path());
    std::vector<uint8_t> bgra = rgba;
    for (size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
    if (!output) throw std::runtime_error("Could not save fluid image");
}
}

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    try
    {
    constexpr uint32_t Width = 128, Height = 128;
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Particle render regression", Width, Height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), Width, Height))
    {
        std::cout << "SKIP: no DirectX 11 device\n";
        return 0;
    }
    if (!renderer.EnableOffscreenExport(Width, Height, 1, 2))
        throw std::runtime_error("Could not initialize particle capture");

    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    auto* cameraObject = scene.AddObject("Camera");
    cameraObject->transform.position = {0.f, 0.f, -3.f};
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    auto* particleObject = scene.AddObject("Particles");
    auto* emitter = particleObject->AddComponent<Engine::Components::ParticleEmitter>();
    emitter->rate = 0.f;
    emitter->burstOnStart = 1;
    emitter->speed = 0.f;
    emitter->gravity = {0.f, 0.f, 0.f};
    emitter->size = 0.6f;
    emitter->color = {0.f, 1.f, 0.f};

    const auto capture = [&](Engine::Components::ParticleEmitter::GpuSnapshot*
        snapshot = nullptr)
    {
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto context = renderer.CreateFrameGraphicsContext();
        if (!context) throw std::runtime_error("No particle render context");
        scene.RecordGpuParticleSimulation(context.get());
        if (snapshot && !emitter->RequestGpuSnapshot(context.get()))
            throw std::runtime_error("GPU particle snapshot request failed");
        scene.PrepareRenderFrame();
        scene.Render(context.get(), 1.f, camera, false, Width, Height);
        renderer.EndFrame();
        renderer.WaitIdle();
        if (snapshot && !emitter->ResolveGpuSnapshot(*snapshot))
            throw std::runtime_error("GPU particle snapshot resolve failed");
        std::vector<uint8_t> pixels;
        if (!renderer.ReadExportFrameRGBA(pixels, true) ||
            pixels.size() != Width * Height * 4u)
            throw std::runtime_error("Particle capture failed");
        return pixels;
    };

    const auto empty = capture();
    scene.Start();
    scene.Update(1.f / 60.f);
    if (emitter->GetParticles().size() != 1)
        throw std::runtime_error("Particle did not spawn");
    const auto rendered = capture();
    const size_t center = ((Height / 2u) * Width + Width / 2u) * 4u;
    if (rendered[center + 1u] < empty[center + 1u] + 20u)
        throw std::runtime_error("Particle did not appear at the image center");
    std::cout << "Particle center green channel: "
        << static_cast<int>(empty[center + 1u]) << " -> "
        << static_cast<int>(rendered[center + 1u]) << '\n';

    emitter->fluidEnabled = true;
    emitter->fluidVisual = true;
    emitter->color = {0.1f, 0.45f, 1.f};
    emitter->Start();
    scene.Update(1.f / 60.f);
    const auto fluidRendered = capture();
    int blueEdgePixels = 0;
    int maximumBlueContrast = 0;
    for (size_t index = 0; index < fluidRendered.size(); index += 4u)
    {
        const int contrast = (static_cast<int>(fluidRendered[index + 2u]) -
            static_cast<int>(fluidRendered[index])) -
            (static_cast<int>(empty[index + 2u]) -
                static_cast<int>(empty[index]));
        maximumBlueContrast = std::max(maximumBlueContrast, contrast);
        if (contrast > 5) ++blueEdgePixels;
    }
    const int centerChange = std::abs(static_cast<int>(fluidRendered[center]) -
        static_cast<int>(empty[center])) +
        std::abs(static_cast<int>(fluidRendered[center + 1u]) -
            static_cast<int>(empty[center + 1u])) +
        std::abs(static_cast<int>(fluidRendered[center + 2u]) -
            static_cast<int>(empty[center + 2u]));
    if (blueEdgePixels < 300 || maximumBlueContrast < 12 ||
        centerChange > 25)
        throw std::runtime_error("Fluid water should have a blue rim and clear center");
    emitter->burstOnStart = 64;
    emitter->maxParticles = 64;
    emitter->shape = static_cast<int>(
        Engine::Components::ParticleEmitter::Shape::Box);
    emitter->boxExtents = {0.08f, 0.45f, 0.05f};
    emitter->size = 0.18f;
    emitter->fluid.pressureStiffness = 0.f;
    emitter->Start();
    scene.Update(1.f / 60.f);
    if (emitter->GetParticles().size() != 64)
        throw std::runtime_error("Fluid cluster did not spawn");
    SaveFluidImage(capture(), Width, Height);
    emitter->burstOnStart = 600;
    emitter->maxParticles = 600;
    emitter->Start();
    scene.Update(1.f / 60.f);
    if (emitter->GetParticles().size() != 600)
        throw std::runtime_error("Dense fluid cluster did not spawn");
    const auto denseFluid = capture();
    if (denseFluid[center + 2u] <= denseFluid[center] + 20u)
        throw std::runtime_error("Dense fluid lost its water surface");
    emitter->fluidEnabled = false;
    emitter->color = {0.f, 1.f, 0.f};
    emitter->burstOnStart = 1;
    emitter->maxParticles = 1024;
    emitter->shape = static_cast<int>(
        Engine::Components::ParticleEmitter::Shape::Point);
    emitter->size = 0.6f;
    emitter->Start();
    scene.Update(1.f / 60.f);

    const auto cpuBounds = emitter->GetWorldBounds(glm::mat4(1.f));
    emitter->useGpuSimulation = true;
    emitter->Start();
    scene.Update(1.f / 60.f);
    Engine::Components::ParticleEmitter::GpuSnapshot gpuSnapshot;
    const auto gpuRendered = capture(&gpuSnapshot);
    if (!emitter->IsGpuSimulationActive() ||
        gpuRendered[center + 1u] < empty[center + 1u] + 20u)
        throw std::runtime_error("GPU particle did not render from storage buffer");
    if (gpuSnapshot.liveCount != 1 || !gpuSnapshot.bounds.valid ||
        gpuSnapshot.minimumAge != 0.f || gpuSnapshot.maximumAge != 0.f ||
        std::abs(gpuSnapshot.bounds.minimum.x - cpuBounds.minimum.x) > 1e-4f ||
        std::abs(gpuSnapshot.bounds.maximum.x - cpuBounds.maximum.x) > 1e-4f)
        throw std::runtime_error("GPU count, age, or bounds snapshot failed");
    emitter->useGpuSimulation = false;

    const auto texturePath = std::filesystem::temp_directory_path() /
        ("EngineParticleRendering_" + std::to_string(GetCurrentProcessId()) + ".tga");
    const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x20,
        0, 0, 255, 255, 0, 0, 255, 255,
        0, 0, 255, 255, 0, 0, 255, 255
    };
    {
        std::ofstream output(texturePath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(tga), sizeof(tga));
        if (!output) throw std::runtime_error("Could not create particle test texture");
    }
    emitter->texturePath = texturePath.string();
    emitter->color = {1.f, 1.f, 1.f};
    emitter->Start();
    scene.Update(1.f / 60.f);
    const auto textured = capture();
    if (textured[center] < empty[center] + 20u ||
        textured[center + 1u] > empty[center + 1u] + 5u)
        throw std::runtime_error("Particle texture did not tint the billboard red");

    emitter->opacity = 0.f;
    const auto transparent = capture();
    for (size_t channel = 0; channel < 3; ++channel)
        if (std::abs(static_cast<int>(transparent[center + channel]) -
            static_cast<int>(empty[center + channel])) > 2)
            throw std::runtime_error("Zero opacity particle changed the image");

    emitter->opacity = 1.f;
    emitter->blendMode = static_cast<int>(
        Engine::Components::ParticleEmitter::BlendMode::Alpha);
    const auto alphaBlended = capture();
    if (alphaBlended[center] < empty[center] + 20u)
        throw std::runtime_error("Alpha-blended particle did not appear");

    emitter->texturePath.clear();
    emitter->blendMode = static_cast<int>(
        Engine::Components::ParticleEmitter::BlendMode::Additive);
    emitter->color = {0.f, 1.f, 0.f};
    emitter->size = 0.4f;
    particleObject->transform.position = {10.f, 0.f, 9.f};
    emitter->Start();
    scene.Update(1.f / 60.f);
    const auto outsideMainView = capture();
    if (outsideMainView[center + 1u] > empty[center + 1u] + 5u)
        throw std::runtime_error("Portal test particle remained in main view");

    auto* entranceObject = scene.AddObject("Particle portal entrance");
    auto* entrance = entranceObject->AddComponent<
        Engine::Components::SpatialManipulator>();
    entrance->portalNormal = {0.f, 0.f, -1.f};
    entrance->connectionMode = static_cast<int>(
        Engine::Components::SpatialManipulator::ConnectionMode::Portal);
    auto* exitObject = scene.AddObject("Particle portal exit");
    exitObject->transform.position = {10.f, 0.f, 8.f};
    auto* exit = exitObject->AddComponent<
        Engine::Components::SpatialManipulator>();
    exit->connectionMode = entrance->connectionMode;
    entrance->ConnectToTarget(exit);
    scene.settings.portalRecursionDepth = 1;
    scene.settings.portalMaxViewsPerFrame = 1;
    scene.Update(1.f / 60.f);
    const auto throughPortal = capture();
    const auto portalParticleCount = scene.GetLastParticlePortalDrawCount();
    particleObject->transform.position.z = 7.f;
    emitter->Start();
    scene.Update(1.f / 60.f);
    const auto clippedAtExit = capture();
    entrance->Disconnect();
    std::filesystem::remove(texturePath);
    if (portalParticleCount == 0)
        throw std::runtime_error("Particle portal draw was not submitted");
    std::cout << "Portal particle center green channel: "
        << static_cast<int>(outsideMainView[center + 1u]) << " -> "
        << static_cast<int>(throughPortal[center + 1u]) << '\n';
    if (throughPortal[center + 1u] < outsideMainView[center + 1u] + 20u)
        throw std::runtime_error("Particle did not appear through portal view");
    if (clippedAtExit[center + 1u] + 20u > throughPortal[center + 1u])
        throw std::runtime_error("Particle behind exit plane was not clipped");
    return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
