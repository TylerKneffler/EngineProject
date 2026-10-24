#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void WriteTexture(const std::filesystem::path& path, uint8_t red, uint8_t blue)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + 16;
    info.biSize = sizeof(info); info.biWidth = 2; info.biHeight = 2;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int i = 0; i < 4; ++i)
    {
        const uint8_t pixel[] = {blue, 0, red, 255};
        out.write(reinterpret_cast<const char*>(pixel), 4);
    }
}
}

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    constexpr unsigned Size = 128;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Terrain viewport regression", Size, Size);
    Engine::Renderers::DX11GameRenderer renderer;
    if (!renderer.Init(window.GetHWND(), Size, Size))
    {
        std::cout << "SKIP: no DirectX 11 device\n";
        return 0;
    }
    if (!renderer.EnableOffscreenExport(Size, Size, 1, 2))
        throw std::runtime_error("Could not create terrain capture");

    const auto assetDir = std::filesystem::temp_directory_path() / "engine-terrain-viewport-test";
    std::filesystem::create_directories(assetDir);
    WriteTexture(assetDir / "red.bmp", 255, 0);
    WriteTexture(assetDir / "blue.bmp", 0, 255);
    for (const char* color : {"red", "blue"})
    {
        std::ofstream material(assetDir / (std::string(color) + ".material"));
        material << "{\"baseColorTexture\":\"" << color << ".bmp\"}";
    }

    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    auto* cameraObject = scene.AddObject("Camera");
    cameraObject->transform.position = {0.f, 0.f, -7.f};
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    auto* terrainObject = scene.AddObject("Terrain");
    terrainObject->transform.position = {-2.f, -2.f, 0.f};
    terrainObject->transform.rotation.x = -1.570796327f;
    auto* terrain = terrainObject->AddComponent<Engine::Components::Terrain>();
    terrain->layers[0].material = (assetDir / "red.material").string();
    terrain->layers.push_back({(assetDir / "blue.material").string(), 0.f, 1.f,
        {0.f, 0.f, 1.f}});
    terrain->Resize(4, 4, 1.f, 1.f);
    for (int row = 0; row <= 4; ++row)
        for (int col = 0; col <= 4; ++col)
        {
            const size_t index = static_cast<size_t>(row * 5 + col) * 2;
            terrain->weights[index] = col < 2 ? 1.f : 0.f;
            terrain->weights[index + 1] = col < 2 ? 0.f : 1.f;
        }
    terrain->OnAfterDeserialize(renderer.GetGraphicsProvider());
    auto* material = terrainObject->AddComponent<Engine::Components::Material>();
    material->unlit = true;

    renderer.BeginExportFrame(1);
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    auto context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    std::vector<uint8_t> pixels;
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Terrain readback failed");
    const auto channel = [&](unsigned x, unsigned y, unsigned component) {
        return pixels[(y * Size + x) * 4 + component];
    };
    if (channel(45, 64, 0) < channel(45, 64, 2) + 30 ||
        channel(83, 64, 2) < channel(83, 64, 0) + 30)
        throw std::runtime_error("Painted terrain materials did not render red and blue regions");
    std::cout << "Terrain red and blue material regions rendered\n";
    std::filesystem::remove_all(assetDir);
}
