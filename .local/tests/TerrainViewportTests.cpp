#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <tuple>
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

// Smooth periodic bumps keep every texture edge tileable while exercising
// the normal, green-channel roughness, and red-channel height paths.
void WriteMaterialMap(const std::filesystem::path& path, const char* kind,
    bool secondLayer)
{
    constexpr int Side = 128;
    constexpr float TwoPi = 6.28318530718f;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + Side * Side * 4;
    info.biSize = sizeof(info); info.biWidth = Side; info.biHeight = Side;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = 0; y < Side; ++y)
        for (int x = 0; x < Side; ++x)
        {
            const float phaseX = TwoPi * x / Side;
            const float phaseY = TwoPi * y / Side;
            const float wave = std::sin(phaseX) * std::sin(phaseY);
            const auto encode = [](float value) {
                return static_cast<uint8_t>(std::clamp(
                    std::lround(value * 255.f), 0l, 255l));
            };
            uint8_t pixel[4] = {0, 0, 0, 255};
            if (std::string_view(kind) == "normal")
            {
                const float slope = secondLayer ? .42f : .28f;
                const float dx = -slope * std::cos(phaseX) *
                    std::sin(phaseY);
                const float dy = -slope * std::sin(phaseX) *
                    std::cos(phaseY);
                const float inverseLength = 1.f /
                    std::sqrt(dx * dx + dy * dy + 1.f);
                pixel[0] = encode((inverseLength + 1.f) * .5f);
                pixel[1] = encode((dy * inverseLength + 1.f) * .5f);
                pixel[2] = encode((dx * inverseLength + 1.f) * .5f);
            }
            else if (std::string_view(kind) == "roughness")
                pixel[1] = encode((secondLayer ? .72f : .52f) +
                    .12f * wave); // green channel
            else
                pixel[2] = encode((secondLayer ? .66f : .34f) +
                    .18f * wave); // red channel
            out.write(reinterpret_cast<const char*>(pixel), 4);
        }
}

void SaveCapture(const std::filesystem::path& path,
    const std::vector<uint8_t>& rgba, unsigned side)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + side * side * 4;
    info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(side);
    info.biHeight = -static_cast<LONG>(side);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        const uint8_t bgra[] = {rgba[i + 2], rgba[i + 1], rgba[i], 255};
        out.write(reinterpret_cast<const char*>(bgra), 4);
    }
    if (!out) throw std::runtime_error("Could not save terrain paint capture");
}
}

int main()
{
    try
    {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    constexpr unsigned Scale = 4, Size = 128 * Scale;
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
        const bool secondLayer = std::string_view(color) == "blue";
        for (const char* kind : {"normal", "roughness", "height"})
            WriteMaterialMap(assetDir /
                (std::string(color) + "_" + kind + ".bmp"), kind,
                secondLayer);
        std::ofstream material(assetDir / (std::string(color) + ".material"));
        material << "{\"baseColorTexture\":\"" << color
            << ".bmp\",\"normalTexture\":\"" << color
            << "_normal.bmp\",\"metallicRoughnessTexture\":\"" << color
            << "_roughness.bmp\",\"heightTexture\":\"" << color
            << "_height.bmp\",\"terrainHeightBlendStrength\":0.25}";
    }
    for (const auto& [name, color, albedo] : {
        std::tuple{"grass", "red", "meadow_grass.png"},
        std::tuple{"soil", "blue", "warm_dirt.png"}})
    {
        std::ofstream material(assetDir / (std::string(name) + ".material"));
        material << "{\"baseColorTexture\":\"Assets/Textures/Terrain/"
            << albedo << "\",\"normalTexture\":\"" << color
            << "_normal.bmp\",\"metallicRoughnessTexture\":\"" << color
            << "_roughness.bmp\",\"heightTexture\":\"" << color
            << "_height.bmp\",\"terrainHeightBlendStrength\":0.25}";
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
            terrain->weights[index] = 1.f - static_cast<float>(col) / 4.f;
            terrain->weights[index + 1] = static_cast<float>(col) / 4.f;
        }
    terrain->OnAfterDeserialize(renderer.GetGraphicsProvider());
    for (const auto& layer : terrain->GetLayerMaterials())
        if (!layer || !layer->normalTexture ||
            !layer->metallicRoughnessTexture || !layer->heightTexture ||
            layer->terrainHeightBlendStrength != .25f)
            throw std::runtime_error("Terrain material maps did not load");
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
    if (scene.GetLastOrdinaryDrawCount() == 0)
        throw std::runtime_error("Visible terrain was culled");
    std::vector<uint8_t> pixels;
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Terrain readback failed");
    const auto channel = [&](unsigned x, unsigned y, unsigned component) {
        return pixels[(y * Size + x) * 4 + component];
    };
    if (channel(45 * Scale, 64 * Scale, 0) <
            channel(45 * Scale, 64 * Scale, 2) + 30 ||
        channel(83 * Scale, 64 * Scale, 2) <
            channel(83 * Scale, 64 * Scale, 0) + 30)
        throw std::runtime_error("Painted terrain materials did not render red and blue regions");
    if (channel(64 * Scale, 64 * Scale, 0) < 50 ||
        channel(64 * Scale, 64 * Scale, 2) < 50 ||
        std::abs(static_cast<int>(channel(64 * Scale, 64 * Scale, 0)) -
            static_cast<int>(channel(64 * Scale, 64 * Scale, 2))) > 50)
        throw std::runtime_error("Painted terrain materials did not blend in the viewport");
    const auto heightBlended = pixels;
    for (const auto& layer : terrain->GetLayerMaterials())
        layer->heightTexture.reset();
    renderer.BeginExportFrame(1);
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Height-free terrain readback failed");
    unsigned blendDifference = 0;
    for (unsigned y = 48 * Scale; y < 80 * Scale; ++y)
        for (unsigned x = 48 * Scale; x < 80 * Scale; ++x)
        {
            const size_t offset = (y * Size + x) * 4;
            blendDifference += std::abs(static_cast<int>(heightBlended[offset]) -
                static_cast<int>(pixels[offset]));
        }
    if (blendDifference < 1000)
        throw std::runtime_error("Height map did not affect painted transition");
    auto* sunObject = scene.AddObject("Material test sun");
    sunObject->transform.position = {0.f, 0.f, -4.f};
    auto* sun = sunObject->AddComponent<Engine::Components::Light>();
    sun->lightType = static_cast<int>(Engine::Components::Light::Type::Point);
    sun->intensity = 8.f;
    sun->range = 20.f;
    sun->castsShadows = false;
    material->unlit = false;
    const auto captureLitTerrain = [&] {
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto litContext = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(litContext.get(), 1.f, camera, false, Size, Size);
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> result;
        if (!renderer.ReadExportFrameRGBA(result, true))
            throw std::runtime_error("Lit terrain readback failed");
        return result;
    };
    const auto pixelDifference = [&](const std::vector<uint8_t>& first,
        const std::vector<uint8_t>& second) {
        unsigned difference = 0;
        for (unsigned y = 40 * Scale; y < 88 * Scale; ++y)
            for (unsigned x = 40 * Scale; x < 88 * Scale; ++x)
            {
                const size_t offset = (y * Size + x) * 4;
                for (size_t component = 0; component < 3; ++component)
                    difference += std::abs(static_cast<int>(first[offset + component]) -
                        static_cast<int>(second[offset + component]));
            }
        return difference;
    };
    const auto layers = terrain->GetLayerMaterials();
    const auto firstNormal = layers[0]->normalTexture;
    const auto secondNormal = layers[1]->normalTexture;
    const auto litMapped = captureLitTerrain();
    layers[0]->normalTexture.reset();
    layers[1]->normalTexture.reset();
    const auto withoutNormals = captureLitTerrain();
    if (pixelDifference(litMapped, withoutNormals) < 1000)
        throw std::runtime_error("Normal maps did not affect lit terrain");
    layers[0]->metallicRoughnessTexture.reset();
    layers[1]->metallicRoughnessTexture.reset();
    const auto withoutRoughness = captureLitTerrain();
    std::filesystem::create_directories(
        ".local/performance-logs/terrain-captures");
    SaveCapture(".local/performance-logs/terrain-captures/material_lit_mapped.bmp",
        litMapped, Size);
    SaveCapture(".local/performance-logs/terrain-captures/material_lit_no_normals.bmp",
        withoutNormals, Size);
    SaveCapture(".local/performance-logs/terrain-captures/material_lit_no_normals_or_roughness.bmp",
        withoutRoughness, Size);
    std::cout << "Terrain map pixel differences: normal="
        << pixelDifference(litMapped, withoutNormals) << " roughness="
        << pixelDifference(withoutNormals, withoutRoughness) << '\n';
    if (pixelDifference(withoutNormals, withoutRoughness) < 1000)
        throw std::runtime_error("Roughness maps did not affect lit terrain");
    layers[0]->normalTexture = firstNormal;
    layers[1]->normalTexture = secondNormal;
    sunObject->enabled = false;
    terrainObject->transform.position.x = 1000.f;
    // Paint the same stroke on both sides of a cell border, then compare
    // captures with the right cell at full and half mesh resolution.
    cameraObject->transform.position = {2.f, 0.f, -9.f};
    std::vector<Engine::Components::Terrain*> paintedCells;
    std::vector<Engine::Core::Object*> paintedObjects;
    for (int cell = 0; cell < 2; ++cell)
    {
        auto* paintedObject = scene.AddObject("Painted border cell");
        paintedObject->transform.position = {-2.f + cell * 4.f, -2.f, 0.f};
        paintedObject->transform.rotation.x = -1.570796327f;
        auto* painted = paintedObject->AddComponent<Engine::Components::Terrain>();
        painted->layers[0].material = (assetDir / "grass.material").string();
        painted->layers.push_back({(assetDir / "soil.material").string(),
            0.f, 1.f, {0.f, 0.f, 1.f}});
        painted->Resize(4, 4, 1.f, 1.f);
        if (!painted->ApplyBrush(cell == 0 ? 3.8f : -.2f, 2.f,
            1.5f, 1.f, Engine::Components::Terrain::Brush::Paint, 1))
            throw std::runtime_error("Border paint stroke did not reach both cells");
        painted->OnAfterDeserialize(renderer.GetGraphicsProvider());
        paintedObject->AddComponent<Engine::Components::Material>()->unlit = true;
        paintedCells.push_back(painted);
        paintedObjects.push_back(paintedObject);
    }
    for (int row = 0; row <= 4; ++row)
    {
        const float leftWeight = paintedCells[0]->weights[
            static_cast<size_t>(row * 5 + 4) * 2 + 1];
        const float rightWeight = paintedCells[1]->weights[
            static_cast<size_t>(row * 5) * 2 + 1];
        if (std::abs(leftWeight - rightWeight) > .00001f)
            throw std::runtime_error("Paint stroke split at the cell border");
    }
    const auto captureBorder = [&](const char* filename) {
        renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto borderContext = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(borderContext.get(), 1.f, camera, false, Size, Size);
        renderer.EndFrame();
        renderer.WaitIdle();
        if (!renderer.ReadExportFrameRGBA(pixels, true))
            throw std::runtime_error("Border paint readback failed");
        const auto captureDir = std::filesystem::path(
            ".local/performance-logs/terrain-captures");
        std::filesystem::create_directories(captureDir);
        SaveCapture(captureDir / filename, pixels, Size);
        return pixels;
    };
    const auto borderLod1 = captureBorder("material_paint_border_lod1.bmp");
    paintedCells[1]->SetMeshLodStride(2);
    const auto borderLod2 = captureBorder("material_paint_border_lod2.bmp");
    unsigned seamDifference = 0, lodDifference = 0;
    for (unsigned y = 180; y < 330; ++y)
    {
        for (unsigned component = 0; component < 3; ++component)
        {
            const size_t left = (y * Size + Size / 2 - 1) * 4 + component;
            seamDifference += std::abs(static_cast<int>(borderLod1[left]) -
                static_cast<int>(borderLod1[left + 4]));
        }
        for (unsigned x = 60; x < 452; ++x)
            for (unsigned component = 0; component < 3; ++component)
            {
                const size_t offset = (y * Size + x) * 4 + component;
                lodDifference += std::abs(static_cast<int>(borderLod1[offset]) -
                    static_cast<int>(borderLod2[offset]));
            }
    }
    const float seamMean = seamDifference / (150.f * 3.f);
    const float lodMean = lodDifference / (150.f * 392.f * 3.f);
    std::cout << "Terrain border pixel deltas: seam=" << seamMean
        << " LOD=" << lodMean << '\n';
    if (seamMean > 12.f || lodMean > 5.f)
        throw std::runtime_error("Painted terrain border or LOD has a visible discontinuity");
    for (auto* paintedObject : paintedObjects)
        paintedObject->transform.position.x = 1000.f;
    auto* indexedObject = scene.AddObject("Offscreen indexed tile");
    auto* indexed = indexedObject->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Components::Mesh::Vertex> tileVertices(4);
    tileVertices[1].pos[0] = 1000.f;
    tileVertices[2].pos[0] = 1001.f;
    tileVertices[3].pos[0] = 1000.f;
    tileVertices[3].pos[2] = 1.f;
    if (!indexed->SetIndexedGeometry(std::move(tileVertices), {1, 2, 3}))
        throw std::runtime_error("Could not build indexed culling fixture");
    indexed->OnAfterDeserialize(renderer.GetGraphicsProvider());
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    if (scene.GetLastOrdinaryDrawCount() != 0)
        throw std::runtime_error("Offscreen terrain geometry reached the draw list");
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Offscreen frame readback failed");
    // The near ridge fills the target's angular span and rises above it.
    // Verify that moving the camera does not require a previous query frame.
    cameraObject->transform.position = {0.f, 4.f, 0.f};
    auto* ridgeObject = scene.AddObject("Occluding ridge");
    ridgeObject->transform.position = {-5.f, 0.f, 4.f};
    auto* ridge = ridgeObject->AddComponent<Engine::Components::Terrain>();
    ridge->Resize(10, 1, 1.f, 1.f);
    std::fill(ridge->heights.begin(), ridge->heights.end(), 3.f);
    ridge->OnAfterDeserialize(renderer.GetGraphicsProvider());
    auto* hiddenObject = scene.AddObject("Hidden terrain");
    hiddenObject->transform.position = {-1.f, 0.f, 9.f};
    auto* hidden = hiddenObject->AddComponent<Engine::Components::Terrain>();
    hidden->Resize(2, 1, 1.f, 0.1f);
    std::fill(hidden->heights.begin(), hidden->heights.end(), 2.f);
    hidden->OnAfterDeserialize(renderer.GetGraphicsProvider());
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    if (scene.GetLastTerrainOcclusionCulledCount() != 1 ||
        scene.GetLastOrdinaryDrawCount() != 1)
        throw std::runtime_error("Terrain behind a ridge was not culled");
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Ridge frame readback failed");
    cameraObject->transform.position.y = 1.f;
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    if (scene.GetLastTerrainOcclusionCulledCount() != 0 ||
        scene.GetLastOrdinaryDrawCount() != 2)
        throw std::runtime_error("Terrain surface was treated as a solid wall");
    if (!renderer.ReadExportFrameRGBA(pixels, true))
        throw std::runtime_error("Low camera frame readback failed");
    cameraObject->transform.position.y = 4.f;
    ridgeObject->transform.position.x = -20.f;
    renderer.BeginFrame();
    renderer.Clear(0.f, 0.f, 0.f);
    context = renderer.CreateFrameGraphicsContext();
    scene.PrepareRenderFrame();
    scene.Render(context.get(), 1.f, camera, false, Size, Size);
    renderer.EndFrame();
    renderer.WaitIdle();
    if (scene.GetLastTerrainOcclusionCulledCount() != 0 ||
        scene.GetLastOrdinaryDrawCount() != 1)
        throw std::runtime_error("Terrain outside the ridge was occluded");
    std::cout << "Terrain red and blue materials blended in the viewport\n";
    std::filesystem::remove_all(assetDir);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Terrain viewport test failed: " << error.what() << '\n';
        return 1;
    }
}
