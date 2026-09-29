#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/IGameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Scripts/TerrainGen/TerrainGen.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <dwmapi.h>

namespace
{
constexpr uint32_t Width = 640;
constexpr uint32_t Height = 360;
constexpr float FixedDeltaTime = 1.f / 60.f;

struct Image
{
    std::vector<uint8_t> bgra;
};

struct ScenarioResult
{
    Image image;
    uint32_t ordinaryDraws = 0;
    uint32_t skinnedObjects = 0;
};

void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

Image CaptureClient(HWND window)
{
    // Vulkan swap-chain images are not exposed through GetDC(window). Capture
    // the DWM-composited desktop rectangle so all three APIs use the same path.
    POINT origin{ 0, 0 };
    ClientToScreen(window, &origin);
    HDC source = GetDC(nullptr);
    HDC destination = CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(Width);
    info.bmiHeader.biHeight = -static_cast<LONG>(Height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS,
        &pixels, nullptr, 0);
    if (!source || !destination || !bitmap || !pixels)
        throw std::runtime_error("Could not allocate the render-test capture surface");
    HGDIOBJ previous = SelectObject(destination, bitmap);
    if (!BitBlt(destination, 0, 0, Width, Height, source, origin.x, origin.y,
        SRCCOPY | CAPTUREBLT))
        throw std::runtime_error("Could not capture the rendered client surface");
    Image image;
    const auto* begin = static_cast<const uint8_t*>(pixels);
    image.bgra.assign(begin, begin + Width * Height * 4u);
    SelectObject(destination, previous);
    DeleteObject(bitmap);
    DeleteDC(destination);
    ReleaseDC(nullptr, source);
    return image;
}

double LuminanceVariance(const Image& image)
{
    double sum = 0.0;
    double squared = 0.0;
    const size_t pixels = image.bgra.size() / 4u;
    for (size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const size_t offset = pixel * 4u;
        const double luminance = image.bgra[offset + 2u] * 0.2126 +
            image.bgra[offset + 1u] * 0.7152 +
            image.bgra[offset] * 0.0722;
        sum += luminance;
        squared += luminance * luminance;
    }
    const double mean = sum / static_cast<double>(pixels);
    return squared / static_cast<double>(pixels) - mean * mean;
}

void SaveDiagnostic(const Image& image, const std::string& backend,
    const std::string& scenePath)
{
    std::filesystem::create_directories(".local/TestResults/render-parity");
    const std::string scene = std::filesystem::path(scenePath).stem().string();
    std::ofstream output(".local/TestResults/render-parity/" + scene + "-" +
        backend + ".ppm", std::ios::binary);
    output << "P6\n" << Width << ' ' << Height << "\n255\n";
    for (size_t offset = 0; offset < image.bgra.size(); offset += 4u)
    {
        const char rgb[3] { static_cast<char>(image.bgra[offset + 2u]),
            static_cast<char>(image.bgra[offset + 1u]),
            static_cast<char>(image.bgra[offset]) };
        output.write(rgb, sizeof(rgb));
    }
}

struct Difference
{
    double mean = 0.0;
    double maximum = 0.0;
    double largeFraction = 0.0;
};

Difference Compare(const Image& first, const Image& second)
{
    if (first.bgra.size() != second.bgra.size() || first.bgra.empty())
        throw std::runtime_error("Render-test captures have incompatible sizes");
    uint64_t total = 0;
    uint64_t large = 0;
    uint8_t maximum = 0;
    const size_t channelCount = first.bgra.size() / 4u * 3u;
    for (size_t offset = 0; offset < first.bgra.size(); offset += 4u)
    {
        for (size_t channel = 0; channel < 3u; ++channel)
        {
            const uint8_t delta = static_cast<uint8_t>(std::abs(
                static_cast<int>(first.bgra[offset + channel]) -
                static_cast<int>(second.bgra[offset + channel])));
            total += delta;
            maximum = std::max(maximum, delta);
            if (delta > 48u) ++large;
        }
    }
    return { static_cast<double>(total) / channelCount,
        static_cast<double>(maximum), static_cast<double>(large) / channelCount };
}

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

Engine::Core::Object* AddAlphaMaskAndMorphFixture(Engine::Scene::Scene& scene,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Engine::Components::Mesh* selectedMesh = nullptr;
    Engine::Components::Material* selectedMaterial = nullptr;
    bool directionalShadow = false;
    for (const auto& object : scene.GetObjects())
    {
        if (!selectedMesh)
            selectedMesh = object->GetComponent<Engine::Components::Mesh>();
        if (!selectedMaterial)
            selectedMaterial = object->GetComponent<Engine::Components::Material>();
        if (const auto* light = object->GetComponent<Engine::Components::Light>())
            directionalShadow |= light->GetLightType() ==
                Engine::Components::Light::Type::Directional &&
                light->castsShadows;
    }
    Require(selectedMesh && selectedMaterial,
        "Lighting fixture does not contain a mesh/material pair");
    Require(directionalShadow,
        "Lighting fixture does not contain a shadow-casting directional light");
    selectedMaterial->alphaMode = "Mask";
    selectedMaterial->alphaCutoff = 0.45f;
    selectedMaterial->castsShadows = true;
    selectedMaterial->SetBaseColorTexture(
        "Assets/Textures/Sprites/Slime/slime_idle1.png");
    selectedMaterial->PrepareTextures(graphicsProvider);

    Engine::Model::MorphTarget target;
    target.positions.resize(selectedMesh->GetVertexCount(), glm::vec3(0.f));
    target.normals.resize(selectedMesh->GetVertexCount(), glm::vec3(0.f));
    target.tangents.resize(selectedMesh->GetVertexCount(), glm::vec3(0.f));
    for (size_t vertex = 0; vertex < target.positions.size(); ++vertex)
        target.positions[vertex].y = (vertex % 2u == 0u) ? 0.15f : -0.05f;
    selectedMesh->SetMorphData(0u, { std::move(target) }, { 0.65f });
    Require(selectedMesh->HasMorphTargets(),
        "Morph fixture did not publish its GPU morph stream");
    Engine::Core::Object* movingCaster = scene.FindObjectByName("Glossy Metal");
    Require(movingCaster != nullptr, "Moving shadow caster fixture is missing");
    return movingCaster;
}

ScenarioResult RenderScenario(const std::string& backend,
    const std::string& scenePath, bool configureLightingFixture,
    bool captureOutput = true)
{
    Engine::Model::ProjectSettings settings{};
    settings.gameRenderingAPI = backend;
    settings.realtimeShadows.enabled = true;
    settings.realtimeShadows.directionalResolution = 1024u;
    settings.realtimeShadows.directionalCascadeCount = 2u;
    settings.realtimeShadows.pcfRadius = 1u;

    const std::wstring title(backend.begin(), backend.end());
    Engine::Core::Window window(GetModuleHandleW(nullptr), title.c_str(),
        Width, Height);
    auto renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
    Require(renderer && renderer->Init(window.GetHWND(), Width, Height),
        backend + " renderer initialization failed");
    Engine::Scene::Scene scene;
    scene.Init(renderer->GetGraphicsProvider());
    scene.SetRealtimeShadowSettings(settings.realtimeShadows);
    scene.SetDistanceLightingSettings(settings.distanceLighting);
    Require(scene.Load(scenePath), "Could not load render fixture: " + scenePath);
    Engine::Core::Object* movingCaster = configureLightingFixture
        ? AddAlphaMaskAndMorphFixture(scene, renderer->GetGraphicsProvider())
        : nullptr;
    scene.Start();
    ShowWindow(window.GetHWND(), SW_SHOWNOACTIVATE);
    SetWindowPos(window.GetHWND(), HWND_TOP, 16, 16, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE);

    TerrainGen* terrainGenerator = nullptr;
    for (const auto& object : scene.GetObjects())
        if (!terrainGenerator)
            terrainGenerator = object->GetComponent<TerrainGen>();
    int frame = 0;
    for (; frame < 600; ++frame)
    {
        PumpMessages();
        if (movingCaster && frame == 45)
        {
            movingCaster->transform.position.x += 0.75f;
            movingCaster->transform.MarkDirty();
        }
        scene.Update(FixedDeltaTime);
        scene.PrepareRenderFrame();
        renderer->BeginFrame();
        renderer->Clear(0.02f, 0.025f, 0.04f, 1.f);
        auto context = renderer->CreateFrameGraphicsContext();
        Require(context != nullptr, backend + " did not create a frame context");
        Engine::Components::Camera* camera = scene.FindGameCamera();
        Require(camera != nullptr, "Render fixture has no active game camera");
        scene.Render(context.get(), static_cast<float>(Width) / Height,
            camera, false, Width, Height);
        renderer->EndFrame();
        const bool minimumFramesRendered = frame >= 89;
        const bool terrainStable = !terrainGenerator ||
            (terrainGenerator->GetLoadedChunkCount() == 9u &&
             terrainGenerator->GetQueuedChunkCount() == 0u &&
             terrainGenerator->GetInFlightChunkCount() == 0u);
        if (minimumFramesRendered && terrainStable)
            break;
    }
    Require(!terrainGenerator || frame < 600,
        backend + " terrain fixture did not become stable");
    renderer->WaitIdle();
    ScenarioResult result;
    result.ordinaryDraws = scene.GetLastOrdinaryDrawCount();
    result.skinnedObjects = scene.GetLastSkinnedObjectCount();
    Require(result.ordinaryDraws > 0u, backend + " produced no ordinary draws");
    if (!captureOutput)
        return result;
    DwmFlush();
    const Image firstCapture = CaptureClient(window.GetHWND());
    Sleep(50);
    DwmFlush();
    result.image = CaptureClient(window.GetHWND());
    const Difference captureDifference = Compare(firstCapture, result.image);
    Require(captureDifference.mean <= 1.0,
        backend + " client capture was not stable after GPU idle");
    SaveDiagnostic(result.image, backend, scenePath);
    Require(LuminanceVariance(result.image) > 16.0,
        backend + " produced a blank or uncapturable frame for " + scenePath);
    return result;
}

void RequireParity(const char* scenario, const ScenarioResult& reference,
    const ScenarioResult& candidate, const std::string& backend)
{
    const Difference difference = Compare(reference.image, candidate.image);
    std::printf("%s DX11/%s: mean=%.3f max=%.0f large=%.4f\n",
        scenario, backend.c_str(), difference.mean, difference.maximum,
        difference.largeFraction);
    Require(difference.mean <= 20.0 && difference.largeFraction <= 0.12,
        std::string(scenario) + " image parity exceeded tolerance for " + backend);
}
}

int main()
{
    try
    {
        const std::array<std::string, 3> backends{
            "DirectX11", "DirectX12", "Vulkan" };
        for (const std::string& backend : backends)
        {
            std::string reason;
            if (!Engine::Renderers::RendererFactory::IsRendererAvailable(
                backend, &reason))
            {
                std::fprintf(stderr, "SKIP: %s unavailable: %s\n",
                    backend.c_str(), reason.c_str());
                return 77;
            }
        }

        if (GetEnvironmentVariableA(
                "ENGINE_RENDER_TEST_SKIP_CAPTURE", nullptr, 0) > 0)
        {
            for (const std::string& backend : backends)
            {
                std::printf("Smoke rendering FP16 composition with %s...\n",
                    backend.c_str());
                std::fflush(stdout);
                RenderScenario(backend,
                    "Engine/Core/Assets/Scenes/Showcases/lighting_showcase.scene",
                    true, false);
            }
            std::puts("FP16 composition smoke test passed on all backends.");
            return 0;
        }

        std::array<ScenarioResult, 3> lighting;
        std::array<ScenarioResult, 3> animation;
        std::array<ScenarioResult, 3> terrain;
        for (size_t backend = 0; backend < backends.size(); ++backend)
        {
            std::printf("Rendering parity fixtures with %s...\n",
                backends[backend].c_str());
            std::fflush(stdout);
            lighting[backend] = RenderScenario(backends[backend],
                "Engine/Core/Assets/Scenes/Showcases/lighting_showcase.scene", true);
            animation[backend] = RenderScenario(backends[backend],
                "Engine/Core/Assets/Scenes/Showcases/animation_showcase.scene", false);
            terrain[backend] = RenderScenario(backends[backend],
                "Engine/Core/Assets/Scenes/Procedural/terrain_gen.scene", false);
            Require(animation[backend].skinnedObjects > 0u,
                backends[backend] + " did not render the skinned animation fixture");
        }
        for (size_t backend = 1; backend < backends.size(); ++backend)
        {
            RequireParity("directional-shadow/alpha-mask/morph/moving-caster",
                lighting[0], lighting[backend], backends[backend]);
            RequireParity("skinning", animation[0], animation[backend],
                backends[backend]);
            RequireParity("terrain", terrain[0], terrain[backend],
                backends[backend]);
        }
        std::puts("Point-shadow render case: NOT SUPPORTED (directional-only shipped scope)");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Render backend parity failure: %s\n", error.what());
        return 1;
    }
}
