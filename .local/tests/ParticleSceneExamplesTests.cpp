#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Object.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Scripts/Particles/ParticleVfxShowcaseDriver.h"
#include "Scripts/Particles/FluidTestSceneDriver.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Load(Engine::Scene::Scene& scene, const char* name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path()
        .parent_path().parent_path() / "Engine/Core/Assets/Scenes/Showcases" /
        name;
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "example scene file missing");
    const std::string xml(std::istreambuf_iterator<char>{input}, {});
    Require(Engine::Serialization::SceneSerializer::LoadFromString(scene, xml),
        "example scene failed to deserialize");
}

void SaveImage(const std::vector<uint8_t>& rgba, uint32_t width,
    uint32_t height, const char* name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path()
        .parent_path() / "artifacts" / name;
    std::filesystem::create_directories(path.parent_path());
    std::vector<uint8_t> bgra = rgba;
    for (size_t index = 0; index < bgra.size(); index += 4)
        std::swap(bgra[index], bgra[index + 2]);
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
    Require(static_cast<bool>(output), "example screenshot write failed");
}

void CaptureExamples()
{
    constexpr uint32_t Width = 512, Height = 512;
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Particle example regression", Width, Height);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), Width, Height))
    {
        std::cout << "SKIP scene images: no DX11 device\n";
        return;
    }
    Require(renderer.EnableOffscreenExport(Width, Height, 1, 2),
        "example screenshot export unavailable");
    const auto capture = [&](const char* sceneFile, const char* cameraName,
        const char* imageName, int ticks)
    {
        Engine::Scene::Scene scene;
        scene.Init(renderer.GetGraphicsProvider());
        Load(scene, sceneFile);
        for (const auto& object : scene.GetObjects())
            if (object)
                if (auto* mesh = object->GetComponent<
                        Engine::Components::Mesh>())
                    if (mesh->GetVertexCount() && !mesh->GetGraphicsBuffer())
                        mesh->CreateBuffer(renderer.GetGraphicsProvider()
                            ->GetBufferFactory());
        auto* cameraObject = scene.FindObjectByName(cameraName);
        auto* camera = cameraObject ? cameraObject->GetComponent<
            Engine::Components::Camera>() : nullptr;
        Require(camera != nullptr, "example camera missing");
        scene.Start();
        for (int tick = 0; tick < ticks; ++tick)
            scene.Update(1.f / 60.f);
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto context = renderer.CreateFrameGraphicsContext();
        Require(context != nullptr, "example render context unavailable");
        scene.PrepareRenderFrame();
        scene.Render(context.get(), 1.f, camera, false, Width, Height);
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> pixels;
        Require(renderer.ReadExportFrameRGBA(pixels, true) &&
            pixels.size() == Width * Height * 4u,
            "example screenshot capture failed");
        if (std::string_view(sceneFile) == "particle_fluid_test.scene")
        {
            uint32_t poolPixels = 0, streamPixels = 0;
            for (uint32_t y = 140; y < 315; ++y)
                for (uint32_t x = 95; x < 410; ++x)
                {
                    const size_t offset = (static_cast<size_t>(y) * Width + x) * 4u;
                    const bool water = pixels[offset + 2u] >
                        pixels[offset] + 30u &&
                        pixels[offset + 1u] > pixels[offset] + 20u;
                    if (!water) continue;
                    if (y >= 235 && x < 310) ++poolPixels;
                    if (y < 220 && x >= 205 && x < 255) ++streamPixels;
                }
            Require(poolPixels > 5000 && streamPixels > 250,
                "fluid basin screenshot lacks a broad pool or connected stream");
        }
        if (std::string_view(sceneFile) == "particle_fluid_river.scene")
        {
            uint32_t sourcePixels = 0, throatPixels = 0, basinPixels = 0;
            uint32_t streamColumns = 0;
            for (uint32_t x = 0; x < Width; ++x)
            {
                bool hasWater = false;
                for (uint32_t y = 0; y < Height; ++y)
                {
                    const size_t offset = (static_cast<size_t>(y) * Width + x) * 4u;
                    const bool water = pixels[offset + 2u] >
                        pixels[offset] + 30u &&
                        pixels[offset + 1u] > pixels[offset] + 20u;
                    if (!water) continue;
                    hasWater = true;
                    if (x < 210) ++sourcePixels;
                    else if (x < 320) ++throatPixels;
                    else ++basinPixels;
                }
                if (x >= 55 && x < 315 && hasWater) ++streamColumns;
            }
            Require(sourcePixels > 500 && throatPixels > 250 &&
                basinPixels > 150 && streamColumns > 220,
                "river screenshot lacks a continuous stream or downstream pool");
        }
        SaveImage(pixels, Width, Height, imageName);
    };
    capture("particle_showcase.scene", "Particle Gallery Camera",
        "particle-vfx-scene.bmp", 43);
    capture("particle_fluid_test.scene", "Fluid Basin | camera",
        "particle-fluid-scene.bmp", 150);
    capture("particle_fluid_river.scene", "Fluid River | camera",
        "particle-fluid-river-scene.bmp", 420);
}
}

int main()
{
    try
    {
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
        Engine::Scene::Scene vfx;
        Load(vfx, "particle_showcase.scene");
        auto* muzzleObject = vfx.FindObjectByName("Gun Flash | repeated burst");
        auto* tracerObject = vfx.FindObjectByName("Gun Flash | tracer");
        auto* cueObject = vfx.FindObjectByName("Particle Gallery | cue controller");
        auto* muzzle = muzzleObject ? muzzleObject->GetComponent<
            Engine::Components::ParticleEmitter>() : nullptr;
        auto* tracer = tracerObject ? tracerObject->GetComponent<
            Engine::Components::ParticleEmitter>() : nullptr;
        Require(muzzle && tracer && cueObject &&
            cueObject->GetComponent<ParticleVfxShowcaseDriver>(),
            "VFX example is missing its scripted cue");
        vfx.Start();
        vfx.Update(1.f / 60.f);
        Require(muzzle->GetParticles().size() == 15 &&
            tracer->GetParticles().size() == 3,
            "VFX script did not fire initial cue");
        for (int tick = 0; tick < 42; ++tick) vfx.Update(1.f / 60.f);
        Require(!muzzle->GetParticles().empty() &&
            muzzle->GetParticles()[0].id > 15,
            "VFX script did not repeat its cue");

        Engine::Scene::Scene fluid;
        Load(fluid, "particle_fluid_test.scene");
        auto* waterObject = fluid.FindObjectByName("Fluid Basin | water");
        auto* paddleObject = fluid.FindObjectByName(
            "Fluid Basin | moving paddle");
        auto* driverObject = fluid.FindObjectByName(
            "Fluid Basin | cue controller");
        auto* water = waterObject ? waterObject->GetComponent<
            Engine::Components::ParticleEmitter>() : nullptr;
        Require(water && water->fluidEnabled && water->fluidVisual &&
            water->fluidCouplingMode == static_cast<int>(
                Engine::Components::ParticleEmitter::FluidCouplingMode::GameplayImpulse) &&
            paddleObject && driverObject &&
            driverObject->GetComponent<FluidTestSceneDriver>(),
            "fluid example is missing its solver, visual, or driver");
        auto* floorObject = fluid.FindObjectByName("Fluid Basin | floor");
        auto* floorMesh = floorObject ? floorObject->GetComponent<
            Engine::Components::Mesh>() : nullptr;
        Require(floorMesh && floorMesh->GetVertexCount() > 0,
            "fluid example basin mesh did not load");
        const float initialPaddleX = paddleObject->transform.position.x;
        fluid.Start();
        for (int tick = 0; tick < 24; ++tick) fluid.Update(1.f / 60.f);
        Require(water->GetParticles().size() > 140 &&
            water->GetParticles().size() <= 360 &&
            waterObject->transform.position.y > 1.f &&
            paddleObject->transform.position.x != initialPaddleX,
            "fluid example did not run or move its boundary");
        for (const auto& particle : water->GetParticles())
            Require(particle.position.x >= water->fluid.boundsMin.x - 1e-4f &&
                particle.position.x <= water->fluid.boundsMax.x + 1e-4f &&
                particle.position.y >= water->fluid.boundsMin.y - 1e-4f &&
                particle.position.y <= water->fluid.boundsMax.y + 1e-4f,
                "fluid example escaped its basin bounds");

        Engine::Scene::Scene river;
        Load(river, "particle_fluid_river.scene");
        auto* riverObject = river.FindObjectByName("Fluid River | water");
        auto* riverWater = riverObject ? riverObject->GetComponent<
            Engine::Components::ParticleEmitter>() : nullptr;
        Require(riverWater && riverWater->fluidEnabled && riverWater->fluidVisual &&
            river.FindObjectByName("Fluid River | sloped bed") &&
            river.FindObjectByName("Fluid River | low bridge") &&
            river.FindObjectByName("Fluid River | catch basin floor"),
            "fluid river example is incomplete");
        river.Start();
        for (int tick = 0; tick < 420; ++tick)
            river.Update(1.f / 60.f);
        size_t upstream = 0, narrow = 0, basin = 0;
        for (const auto& particle : riverWater->GetParticles())
        {
            if (particle.position.x < -0.6f) ++upstream;
            else if (particle.position.x < 0.55f) ++narrow;
            else ++basin;
        }
        Require(riverWater->GetParticles().size() > 100 && basin > 20 &&
            narrow > 10 && upstream > 20,
            "fluid did not occupy the river source, throat, and basin");

        CaptureExamples();

        std::cout << "VFX and fluid example scenes load and run\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
