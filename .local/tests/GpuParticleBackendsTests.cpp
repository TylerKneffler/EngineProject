#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#endif
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Engine::Components::ParticleEmitter;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void SaveImage(const std::vector<uint8_t>& rgba, const char* name)
{
    constexpr int Width = 128, Height = 128;
    Require(rgba.size() == Width * Height * 4,
        "scene capture size mismatch");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = Width;
    info.bmiHeader.biHeight = -Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(rgba);
    for (size_t index = 0; index < pixels.size(); index += 4)
        std::swap(pixels[index], pixels[index + 2]);
    const auto path = std::filesystem::path(__FILE__).parent_path()
        .parent_path() / "artifacts" /
        (std::string("gpu-particles-") + name + ".bmp");
    std::filesystem::create_directories(path.parent_path());
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader),
        sizeof(BITMAPINFOHEADER));
    output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    Require(static_cast<bool>(output), "window capture file failed");
    std::cout << "IMAGE " << path.string() << '\n';
}

void Run(const char* name, Engine::Renderers::IGameRenderer& renderer,
    HWND hwnd)
{
    constexpr uint32_t Width = 128, Height = 128;
    if (!renderer.Init(hwnd, Width, Height))
    {
        std::cout << "SKIP " << name << ": no backend device\n";
        return;
    }
    if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(
        &renderer))
        Require(dx11->EnableOffscreenExport(Width, Height, 1, 2),
            "DX11 scene capture initialization failed");
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    auto* cameraObject = scene.AddObject("Camera");
    cameraObject->transform.position = {0.f, 0.f, -3.f};
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    auto* particleObject = scene.AddObject("Particles");
    particleObject->transform.position = {0.25f, 0.f, 0.f};
    auto* emitter = particleObject->AddComponent<ParticleEmitter>();
    emitter->useGpuSimulation = true;
    emitter->maxParticles = 2;
    emitter->burstOnStart = 3; // verify bounded allocation
    emitter->rate = 0.f;
    emitter->seed = 17;
    emitter->shape = static_cast<int>(ParticleEmitter::Shape::Point);
    emitter->speed = 0.f;
    emitter->gravity = {0.f, 0.f, 0.f};
    emitter->size = 0.25f;
    emitter->lifetime = 0.05f;
    ParticleEmitter cpu;
    cpu.maxParticles = emitter->maxParticles;
    cpu.burstOnStart = emitter->burstOnStart;
    cpu.rate = emitter->rate;
    cpu.seed = emitter->seed;
    cpu.shape = emitter->shape;
    cpu.speed = emitter->speed;
    cpu.gravity = emitter->gravity;
    cpu.size = emitter->size;
    cpu.lifetime = emitter->lifetime;
    cpu.Start();
    glm::mat4 emitterWorld(1.f);
    emitterWorld[3] = glm::vec4(0.25f, 0.f, 0.f, 1.f);
    scene.Start();

    uint32_t frameIndex = 0;
    const auto frame = [&]()
    {
        const bool captureImage = frameIndex == 0 || frameIndex == 3;
        scene.Update(1.f / 60.f);
        cpu.Advance(1.f / 60.f, emitterWorld);
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(
            &renderer)) dx11->BeginExportFrame(1);
        renderer.BeginFrame();
        auto context = renderer.CreateFrameGraphicsContext();
        Require(context != nullptr, "no frame graphics context");
        scene.RecordGpuParticleSimulation(context.get());
        Require(emitter->IsGpuSimulationActive(),
            "GPU particle path did not activate");
        Require(emitter->RequestGpuSnapshot(context.get()),
            "GPU snapshot request failed");
        renderer.Clear(0.f, 0.f, 0.f);
        scene.PrepareRenderFrame();
        scene.Render(context.get(), 1.f, camera, false, Width, Height);
        if (captureImage)
        {
            if (auto* dx12 = dynamic_cast<Engine::Renderers::DX12GameRenderer*>(
                &renderer)) dx12->RequestSceneCapture();
#if defined(ENGINE_VULKAN_ENABLED)
            if (auto* vulkan = dynamic_cast<Engine::Renderers::VulkanGameRenderer*>(
                &renderer)) vulkan->RequestSceneCapture();
#endif
        }
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> dx11Image;
        if (auto* dx11 = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(
            &renderer)) Require(dx11->ReadExportFrameRGBA(dx11Image, true),
                "DX11 scene capture failed");
        if (captureImage)
        {
            std::vector<uint8_t> image = std::move(dx11Image);
            if (auto* dx12 = dynamic_cast<Engine::Renderers::DX12GameRenderer*>(
                &renderer)) Require(dx12->ReadSceneCaptureRGBA(image),
                    "DX12 scene capture failed");
#if defined(ENGINE_VULKAN_ENABLED)
            if (auto* vulkan = dynamic_cast<Engine::Renderers::VulkanGameRenderer*>(
                &renderer)) Require(vulkan->ReadSceneCaptureRGBA(image),
                    "Vulkan scene capture failed");
#endif
            if (!image.empty())
            {
                const std::string imageName = frameIndex == 0 ? name :
                    std::string(name) + "-empty";
                SaveImage(image, imageName.c_str());
            }
        }
        ++frameIndex;
        ParticleEmitter::GpuSnapshot result;
        Require(emitter->ResolveGpuSnapshot(result),
            "GPU snapshot resolve failed");
        const auto cpuBounds = cpu.GetWorldBounds(emitterWorld);
        Require(result.liveCount == cpu.GetParticles().size() &&
            result.bounds.valid == cpuBounds.valid,
            "GPU/CPU fixed-seed count or bounds validity mismatch");
        if (cpuBounds.valid)
        {
            Require(std::abs(result.bounds.minimum.x - cpuBounds.minimum.x) < 1e-4f &&
                std::abs(result.bounds.maximum.x - cpuBounds.maximum.x) < 1e-4f &&
                std::abs(result.bounds.minimum.y - cpuBounds.minimum.y) < 1e-4f &&
                std::abs(result.bounds.maximum.y - cpuBounds.maximum.y) < 1e-4f &&
                std::abs(result.minimumAge - cpu.GetParticles()[0].age) < 1e-4f,
                "GPU/CPU fixed-seed age or bounds mismatch");
        }
        return result;
    };

    const auto spawned = frame();
    Require(spawned.liveCount == 2 && spawned.bounds.valid,
        "GPU spawn/budget count mismatch");
    Require(std::abs(spawned.bounds.minimum.x -
        (0.25f - 0.25f * 1.41421356237f)) < 1e-4f,
        "GPU spawn bounds mismatch");
    Require(spawned.maximumAge == 0.f, "GPU spawn age mismatch");
    const auto integrated = frame();
    Require(integrated.liveCount == 2 && integrated.bounds.valid &&
        std::abs(integrated.minimumAge - 1.f / 60.f) < 1e-4f,
        "GPU integration count or age mismatch");
    frame();
    const auto expired = frame();
    Require(expired.liveCount == 0 && !expired.bounds.valid,
        "GPU expiry/compaction mismatch");
    std::cout << "PASS " << name << " GPU spawn, age, bounds, expiry, render\n";
}
}

int main(int argc, char** argv)
{
    try
    {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"GPU particle backend regression", 128, 128);
    const std::string selected = argc > 1 ? argv[1] : "DX11";
    if (selected == "DX11")
    {
        Engine::Renderers::DX11GameRenderer dx11;
        Run("DX11", dx11, window.GetHWND());
    }
    else if (selected == "DX12")
    {
        Engine::Renderers::DX12GameRenderer dx12;
        Run("DX12", dx12, window.GetHWND());
    }
#if defined(ENGINE_VULKAN_ENABLED)
    else if (selected == "Vulkan")
    {
        Engine::Renderers::VulkanGameRenderer vulkan;
        Run("Vulkan", vulkan, window.GetHWND());
    }
#endif
    }
    catch (const std::exception& error)
    {
        std::cerr << "GPU backend regression: " << error.what() << std::endl;
        return 1;
    }
}
