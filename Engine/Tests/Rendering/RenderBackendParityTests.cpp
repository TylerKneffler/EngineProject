#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
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
#include <functional>
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

using SceneConfiguration = std::function<void(Engine::Scene::Scene&)>;

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
    const std::string& scenePath, const std::string& variant)
{
    std::filesystem::create_directories(".local/TestResults/render-parity");
    const std::string scene = std::filesystem::path(scenePath).stem().string();
    std::ofstream output(".local/TestResults/render-parity/" + scene + "-" +
        variant + "-" + backend + ".ppm", std::ios::binary);
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

struct RegionDifference
{
    double changedFraction = 0.0;
    double boundingFraction = 0.0;
    double changedLuminanceVariance = 0.0;
};

RegionDifference CompareRegion(const Image& first, const Image& second)
{
    if (first.bgra.size() != second.bgra.size() || first.bgra.empty())
        throw std::runtime_error("Portal captures have incompatible sizes");
    uint32_t minX = Width, minY = Height, maxX = 0u, maxY = 0u;
    uint64_t changed = 0u;
    double sum = 0.0, squared = 0.0;
    for (uint32_t y = 0; y < Height; ++y)
    {
        for (uint32_t x = 0; x < Width; ++x)
        {
            const size_t offset = (static_cast<size_t>(y) * Width + x) * 4u;
            const int db = std::abs(static_cast<int>(first.bgra[offset]) -
                static_cast<int>(second.bgra[offset]));
            const int dg = std::abs(static_cast<int>(first.bgra[offset + 1u]) -
                static_cast<int>(second.bgra[offset + 1u]));
            const int dr = std::abs(static_cast<int>(first.bgra[offset + 2u]) -
                static_cast<int>(second.bgra[offset + 2u]));
            if (std::max({ db, dg, dr }) <= 12)
                continue;
            minX = std::min(minX, x); minY = std::min(minY, y);
            maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            const double luminance = first.bgra[offset + 2u] * 0.2126 +
                first.bgra[offset + 1u] * 0.7152 + first.bgra[offset] * 0.0722;
            sum += luminance;
            squared += luminance * luminance;
            ++changed;
        }
    }
    RegionDifference result;
    const double pixelCount = static_cast<double>(Width) * Height;
    result.changedFraction = static_cast<double>(changed) / pixelCount;
    if (changed != 0u)
    {
        result.boundingFraction = static_cast<double>(maxX - minX + 1u) *
            (maxY - minY + 1u) / pixelCount;
        const double mean = sum / static_cast<double>(changed);
        result.changedLuminanceVariance =
            squared / static_cast<double>(changed) - mean * mean;
    }
    return result;
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
    bool captureOutput = true, const std::string& variant = "default",
    const SceneConfiguration& configureScene = {})
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
    if (configureScene)
        configureScene(scene);
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
    SaveDiagnostic(result.image, backend, scenePath, variant);
    Require(LuminanceVariance(result.image) > 16.0,
        backend + " produced a blank or uncapturable frame for " + scenePath);
    return result;
}

void SetPortalsEnabled(Engine::Scene::Scene& scene, bool enabled)
{
    for (const auto& object : scene.GetObjects())
        if (auto* portal = object->GetComponent<
            Engine::Components::SpatialManipulator>())
            portal->enabled = enabled;
}

void ConfigurePortalOccluder(Engine::Scene::Scene& scene)
{
    Engine::Core::Object* wall = scene.FindObjectByName("Scale Test Cube");
    Require(wall != nullptr, "Portal occlusion fixture has no mesh");
    wall->transform.position = { 7.f, 4.f, -8.f };
    wall->transform.rotation = { 0.f, 0.f, 0.f };
    wall->transform.scale = { 20.f, 20.f, 1.f };
    wall->transform.MarkDirty();
    if (auto* rigidBody = wall->GetComponent<Engine::Components::RigidBody>())
    {
        rigidBody->bodyType = "Static";
        rigidBody->initialLinearVelocity = glm::vec3(0.f);
        rigidBody->initialAngularVelocity = glm::vec3(0.f);
    }
}

void RequirePortalFrameValidation(const std::string& backend,
    const ScenarioResult& visible, const ScenarioResult& disabled,
    const ScenarioResult& shallow, const ScenarioResult& recursive,
    const ScenarioResult& occluded, const ScenarioResult& occludedDisabled)
{
    const RegionDifference aperture = CompareRegion(visible.image, disabled.image);
    std::printf("portal aperture %s: changed=%.4f bounds=%.4f variance=%.2f\n",
        backend.c_str(), aperture.changedFraction, aperture.boundingFraction,
        aperture.changedLuminanceVariance);
    Require(aperture.changedFraction >= 0.001 && aperture.changedFraction <= 0.30,
        backend + " portal output was absent or escaped its aperture");
    Require(aperture.boundingFraction <= 0.45,
        backend + " portal depth/stencil mask affected too much of the frame");
    Require(aperture.changedLuminanceVariance >= 20.0,
        backend + " portal did not show distinct target-side content");

    const RegionDifference recursion = CompareRegion(recursive.image, shallow.image);
    Require(recursion.changedFraction >= 0.00005,
        backend + " recursive portal depth produced no nested-view pixels");
    Require(recursion.changedFraction <= aperture.boundingFraction + 0.01,
        backend + " recursive portal view escaped the root aperture");

    const RegionDifference hidden = CompareRegion(
        occluded.image, occludedDisabled.image);
    Require(hidden.changedFraction <= 0.0005,
        backend + " portal remote view leaked through occluding geometry");
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
                std::printf("Smoke rendering recursive portals with %s...\n",
                    backend.c_str());
                std::fflush(stdout);
                RenderScenario(backend,
                    "Engine/Core/Assets/Scenes/Portals/portal_size_ratio.scene",
                    false, false, "portal-smoke",
                    [](Engine::Scene::Scene& scene)
                    {
                        scene.settings.portalRecursionDepth = 4;
                        scene.settings.portalConnectionRepeatLimit = 4;
                    });
            }
            std::puts("FP16 composition and recursive portal smoke tests passed on all backends.");
            return 0;
        }

        std::array<ScenarioResult, 3> lighting;
        std::array<ScenarioResult, 3> animation;
        std::array<ScenarioResult, 3> terrain;
        std::array<ScenarioResult, 3> portalVisible;
        std::array<ScenarioResult, 3> portalDisabled;
        std::array<ScenarioResult, 3> portalShallow;
        std::array<ScenarioResult, 3> portalRecursive;
        std::array<ScenarioResult, 3> portalOccluded;
        std::array<ScenarioResult, 3> portalOccludedDisabled;
        constexpr const char* portalScene =
            "Engine/Core/Assets/Scenes/Portals/portal_size_ratio.scene";
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
            portalVisible[backend] = RenderScenario(backends[backend], portalScene,
                false, true, "portal-visible");
            portalDisabled[backend] = RenderScenario(backends[backend], portalScene,
                false, true, "portal-disabled", [](Engine::Scene::Scene& scene)
                {
                    SetPortalsEnabled(scene, false);
                });
            portalShallow[backend] = RenderScenario(backends[backend], portalScene,
                false, true, "portal-depth-1", [](Engine::Scene::Scene& scene)
                {
                    scene.settings.portalRecursionDepth = 1;
                });
            portalRecursive[backend] = RenderScenario(backends[backend], portalScene,
                false, true, "portal-depth-4", [](Engine::Scene::Scene& scene)
                {
                    scene.settings.portalRecursionDepth = 4;
                    scene.settings.portalConnectionRepeatLimit = 4;
                });
            portalOccluded[backend] = RenderScenario(backends[backend], portalScene,
                false, true, "portal-occluded", [](Engine::Scene::Scene& scene)
                {
                    ConfigurePortalOccluder(scene);
                });
            portalOccludedDisabled[backend] = RenderScenario(backends[backend],
                portalScene, false, true, "portal-occluded-disabled",
                [](Engine::Scene::Scene& scene)
                {
                    ConfigurePortalOccluder(scene);
                    SetPortalsEnabled(scene, false);
                });
            Require(animation[backend].skinnedObjects > 0u,
                backends[backend] + " did not render the skinned animation fixture");
            RequirePortalFrameValidation(backends[backend], portalVisible[backend],
                portalDisabled[backend], portalShallow[backend],
                portalRecursive[backend], portalOccluded[backend],
                portalOccludedDisabled[backend]);
        }
        for (size_t backend = 1; backend < backends.size(); ++backend)
        {
            RequireParity("directional-shadow/alpha-mask/morph/moving-caster",
                lighting[0], lighting[backend], backends[backend]);
            RequireParity("skinning", animation[0], animation[backend],
                backends[backend]);
            RequireParity("terrain", terrain[0], terrain[backend],
                backends[backend]);
            RequireParity("portal-visible", portalVisible[0],
                portalVisible[backend], backends[backend]);
            RequireParity("portal-recursive", portalRecursive[0],
                portalRecursive[backend], backends[backend]);
            RequireParity("portal-occluded", portalOccluded[0],
                portalOccluded[backend], backends[backend]);
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
