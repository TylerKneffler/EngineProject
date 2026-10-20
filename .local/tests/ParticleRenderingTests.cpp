#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main()
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

    const auto capture = [&]()
    {
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto context = renderer.CreateFrameGraphicsContext();
        if (!context) throw std::runtime_error("No particle render context");
        scene.Render(context.get(), 1.f, camera, false, Width, Height);
        renderer.EndFrame();
        renderer.WaitIdle();
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
    std::filesystem::remove(texturePath);
}
