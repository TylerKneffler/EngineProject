#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t Width = 640u;
constexpr uint32_t Height = 360u;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "shadow regression failed: %s (%s:%d)\n", \
        #condition, __FILE__, __LINE__); return false; } } while (false)

void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template<typename Component>
Component* FindFirst(Engine::Scene::Scene& scene)
{
    for (const auto& object : scene.GetObjects())
        if (object)
            if (auto* component = object->GetComponent<Component>())
                return component;
    return nullptr;
}

struct ImageMetrics
{
    double mean = 0.0;
    double variance = 0.0;
    uint64_t hash = 1469598103934665603ull;
};

ImageMetrics Measure(const std::vector<uint8_t>& pixels)
{
    ImageMetrics result{};
    if (pixels.empty())
        return result;
    const size_t count = pixels.size() / 4u;
    double squared = 0.0;
    for (size_t pixel = 0; pixel < count; ++pixel)
    {
        const uint8_t* rgba = pixels.data() + pixel * 4u;
        const double luminance = 0.2126 * rgba[0] + 0.7152 * rgba[1] +
            0.0722 * rgba[2];
        result.mean += luminance;
        squared += luminance * luminance;
        for (size_t channel = 0; channel < 4u; ++channel)
        {
            result.hash ^= rgba[channel];
            result.hash *= 1099511628211ull;
        }
    }
    result.mean /= static_cast<double>(count);
    result.variance = squared / static_cast<double>(count) -
        result.mean * result.mean;
    return result;
}

double MeanDifference(const std::vector<uint8_t>& first,
    const std::vector<uint8_t>& second)
{
    if (first.size() != second.size() || first.empty())
        return 255.0;
    uint64_t difference = 0;
    for (size_t index = 0; index < first.size(); index += 4u)
        for (size_t channel = 0; channel < 3u; ++channel)
            difference += static_cast<uint64_t>(std::abs(
                static_cast<int>(first[index + channel]) -
                static_cast<int>(second[index + channel])));
    return static_cast<double>(difference) /
        static_cast<double>((first.size() / 4u) * 3u);
}

std::vector<Engine::Model::AnimationVertex> ThinTriangle(bool reversed)
{
    const std::array<glm::vec3, 3> points{{
        {-0.7f, 0.f, 0.f}, {0.7f, 0.f, 0.f}, {0.f, 2.4f, 0.f} }};
    const std::array<uint32_t, 3> order = reversed
        ? std::array<uint32_t, 3>{ 2u, 1u, 0u }
        : std::array<uint32_t, 3>{ 0u, 1u, 2u };
    std::vector<Engine::Model::AnimationVertex> vertices(3u);
    for (size_t index = 0; index < vertices.size(); ++index)
    {
        const glm::vec3 point = points[order[index]];
        vertices[index].pos[0] = point.x;
        vertices[index].pos[1] = point.y;
        vertices[index].pos[2] = point.z;
        vertices[index].normal[2] = reversed ? 1.f : -1.f;
        vertices[index].tangent[0] = 1.f;
        vertices[index].tangent[3] = 1.f;
        vertices[index].color[0] = reversed ? 0.25f : 0.9f;
        vertices[index].color[1] = reversed ? 0.9f : 0.3f;
        vertices[index].color[2] = 0.2f;
        vertices[index].color[3] = 1.f;
    }
    return vertices;
}

void AddThinGeometry(Engine::Scene::Scene& scene,
    Engine::Graphics::IGraphicsProvider* provider)
{
    for (int index = 0; index < 2; ++index)
    {
        auto* object = scene.AddObject(index ? "Reversed thin caster" :
            "Thin caster");
        object->transform.position = { index ? 1.4f : -1.4f, 0.f, 0.5f };
        object->transform.scale = { 1.f, 1.f, 0.025f };
        auto* mesh = object->AddComponent<Engine::Components::Mesh>();
        mesh->SetDeformedVertices(ThinTriangle(index != 0));
        mesh->CreateBuffer(provider->GetBufferFactory());
        auto* material = object->AddComponent<Engine::Components::Material>();
        material->doubleSided = index != 0;
        material->castsShadows = true;
        material->roughnessFactor = 1.f;
    }
}

bool RenderCapture(Engine::Scene::Scene& scene,
    Engine::Renderers::DX11GameRenderer& renderer,
    std::vector<uint8_t>& pixels,
    double* submitMilliseconds = nullptr)
{
    PumpMessages();
    scene.PrepareRenderFrame();
    renderer.BeginFrame();
    auto context = renderer.CreateFrameGraphicsContext();
    auto* camera = scene.FindGameCamera();
    CHECK(context && camera);
    const auto begin = std::chrono::steady_clock::now();
    scene.Render(context.get(), static_cast<float>(Width) / Height, camera,
        false, Width, Height, true);
    renderer.Clear(0.02f, 0.025f, 0.035f);
    scene.Render(context.get(), static_cast<float>(Width) / Height, camera,
        false, Width, Height, false);
    const auto end = std::chrono::steady_clock::now();
    renderer.EndFrame();
    renderer.WaitIdle();
    if (submitMilliseconds)
        *submitMilliseconds = std::chrono::duration<double, std::milli>(
            end - begin).count();
    CHECK(renderer.ReadExportFrameRGBA(pixels, true));
    CHECK(pixels.size() == static_cast<size_t>(Width) * Height * 4u);
    return true;
}

bool RunLightBudgetStress()
{
    Engine::Scene::Scene scene;
    for (uint32_t index = 0; index < 96u; ++index)
    {
        auto* object = scene.AddObject("Point " + std::to_string(index));
        object->transform.position = {
            static_cast<float>(index % 12u) - 6.f,
            1.f + static_cast<float>((index / 12u) % 4u),
            static_cast<float>(index / 48u) * 8.f };
        auto* light = object->AddComponent<Engine::Components::Light>();
        light->intensity = 1.f + static_cast<float>(index) * 0.125f;
        light->range = 10.f + static_cast<float>(index % 7u);
    }
    for (uint32_t index = 0; index < 4u; ++index)
    {
        auto* object = scene.AddObject("Directional " + std::to_string(index));
        auto* light = object->AddComponent<Engine::Components::Light>();
        light->lightType = static_cast<int>(
            Engine::Components::Light::Type::Directional);
        light->intensity = 2.f + static_cast<float>(index);
        light->castsShadows = true;
    }

    Engine::Rendering::RealtimeLightingPipeline pipeline;
    std::array<Engine::Model::LightData,
        Engine::Model::MaxRealtimeLights> output{};
    Engine::Model::RealtimeShadowSettings settings{};
    Engine::Model::RealtimeShadowSelection selection{};
    const glm::vec3 importancePosition(0.f);
    uint64_t stableHash = 0u;
    std::string stableSelection;
    for (uint32_t frame = 0; frame < 240u; ++frame)
    {
        const uint32_t count = pipeline.CollectLights(scene, output.data(),
            static_cast<uint32_t>(output.size()), true, &settings,
            &selection, &importancePosition);
        CHECK(count == Engine::Model::MaxRealtimeLights);
        CHECK(selection.valid);
        uint64_t hash = 1469598103934665603ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(output.data());
        for (size_t byte = 0; byte < sizeof(output); ++byte)
        {
            hash ^= bytes[byte];
            hash *= 1099511628211ull;
        }
        if (frame == 0u)
        {
            stableHash = hash;
            stableSelection = selection.stableLightKey;
        }
        CHECK(hash == stableHash);
        CHECK(selection.stableLightKey == stableSelection);
    }
    return true;
}

bool RunSpotAndPhysicalLightRegression()
{
    Engine::Components::Light defaults;
    CHECK(defaults.GetLightType() == Engine::Components::Light::Type::Point);
    CHECK(defaults.GetIntensityMode() ==
        Engine::Components::Light::IntensityMode::Legacy);

    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Physical spot");
    object->transform.rotation = { 0.f, 0.5f, 0.f };
    auto* light = object->AddComponent<Engine::Components::Light>();
    light->lightType = static_cast<int>(Engine::Components::Light::Type::Spot);
    light->intensityMode = static_cast<int>(
        Engine::Components::Light::IntensityMode::Physical);
    light->intensity = 1000.f;
    light->range = 12.f;
    light->innerConeAngle = 20.f;
    light->outerConeAngle = 40.f;
    light->castsShadows = true;
    light->lightingChannels = 0x24;
    light->cookieTexture = "cookie-test.png";
    light->iesProfileTexture = "ies-test.png";

    Engine::Rendering::RealtimeLightingPipeline pipeline;
    Engine::Model::LightData output{};
    Engine::Model::RealtimeShadowSettings settings{};
    Engine::Model::RealtimeShadowSelection shadow{};
    std::array<std::string, 4> cookies{};
    std::array<std::string, 4> profiles{};
    CHECK(pipeline.CollectLights(scene, &output, 1u, false, &settings,
        &shadow, nullptr, &cookies, &profiles) == 1u);
    CHECK(std::abs(output.params.y - 2.f) < 0.001f);
    CHECK(output.attenuation.y == 1.f);
    CHECK(output.directionCone.w < output.attenuation.x);
    const float solidAngle = 2.f * 3.14159265358979323846f *
        (1.f - std::cos(glm::radians(40.f)));
    CHECK(std::abs(output.colorIntensity.w - 1000.f / solidAngle) < 0.01f);
    CHECK(!shadow.valid); // Spot lights cannot consume directional shadow tiles.
    CHECK(glm::floatBitsToUint(output.attenuation.z) == 0x24u);
    CHECK(output.photometry.x == 1.f && cookies[0] == "cookie-test.png");
    CHECK(output.photometry.y == 1.f && profiles[0] == "ies-test.png");
    return true;
}

bool RunVisualRegressions(Engine::Renderers::DX11GameRenderer& renderer)
{
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    CHECK(scene.Load("Engine/Core/Assets/Scenes/Showcases/lighting_showcase.scene"));
    AddThinGeometry(scene, renderer.GetGraphicsProvider());
    Engine::Model::RealtimeShadowSettings settings{};
    settings.directionalResolution = 1024u;
    settings.directionalCascadeCount = 4u;
    settings.directionalDistance = 120.f;
    scene.SetRealtimeShadowSettings(settings);
    scene.Start();

    auto* light = FindFirst<Engine::Components::Light>(scene);
    auto* camera = scene.FindGameCamera();
    CHECK(light && camera && camera->Owner);
    std::vector<uint8_t> baseline, changed, shimmer;
    CHECK(RenderCapture(scene, renderer, baseline));
    const ImageMetrics baselineMetrics = Measure(baseline);
    CHECK(baselineMetrics.variance > 25.0);
    CHECK(scene.GetShadowDebugSnapshot().casterCount >= 2u);
    const uint64_t allocationGeneration =
        scene.GetShadowDebugSnapshot().atlasAllocationGeneration;

    // Bias and reversed/thin geometry must influence the actual shadow frame.
    light->shadowDepthBias = 0.02f;
    light->shadowNormalBias = 0.08f;
    CHECK(RenderCapture(scene, renderer, changed));
    CHECK(MeanDifference(baseline, changed) > 0.005);
    light->shadowDepthBias = 0.0015f;
    light->shadowNormalBias = 0.01f;

    // Cascade seams stay bounded when switching from a single projection to
    // the four-tile atlas, and sub-texel camera motion remains stable.
    settings.directionalCascadeCount = 1u;
    scene.SetRealtimeShadowSettings(settings);
    CHECK(RenderCapture(scene, renderer, changed));
    CHECK(Measure(changed).variance > 25.0);
    settings.directionalCascadeCount = 4u;
    scene.SetRealtimeShadowSettings(settings);
    camera->Owner->transform.position.x += 0.0001f;
    CHECK(RenderCapture(scene, renderer, shimmer));
    camera->Owner->transform.position.x -= 0.0001f;
    CHECK(MeanDifference(baseline, shimmer) < 8.0);

    // Far fade changes the frame without producing a black or invalid image.
    settings.directionalDistance = 8.f;
    scene.SetRealtimeShadowSettings(settings);
    CHECK(RenderCapture(scene, renderer, changed));
    CHECK(Measure(changed).variance > 25.0);
    CHECK(MeanDifference(baseline, changed) > 0.005);

    // Translate the complete fixture into large world coordinates. Relative
    // lighting and shadow structure must remain visually recognizable.
    settings.directionalDistance = 120.f;
    scene.SetRealtimeShadowSettings(settings);
    for (const auto& object : scene.GetObjects())
        if (object)
            object->transform.position.x += 50000.f;
    CHECK(RenderCapture(scene, renderer, changed));
    CHECK(Measure(changed).variance > 20.0);
    CHECK(scene.GetShadowDebugSnapshot().atlasAllocationGeneration ==
        allocationGeneration);

    // Existing portal fixture exercises portal atlas reuse, mapped cameras,
    // and connected-chart casters/lights using the same framebuffer checks.
    Engine::Scene::Scene portalScene;
    portalScene.Init(renderer.GetGraphicsProvider());
    CHECK(portalScene.Load(
        "Engine/Core/Assets/Scenes/Portals/portal_size_ratio.scene"));
    portalScene.SetRealtimeShadowSettings(settings);
    portalScene.Start();
    CHECK(RenderCapture(portalScene, renderer, changed));
    CHECK(Measure(changed).variance > 20.0);
    CHECK(portalScene.GetShadowDebugSnapshot().hasSelectedLight);
    return true;
}

bool RunRenderedBudgetStress(Engine::Renderers::DX11GameRenderer& renderer)
{
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    CHECK(scene.Load(
        "Engine/Core/Assets/Scenes/Rendering/render_lighting_stress.scene"));
    // Keep this fixture at and beyond the hard realtime-light cap even if the
    // authored stress scene is later reduced for interactive readability.
    for (uint32_t index = 0; index < 80u; ++index)
    {
        auto* object = scene.AddObject("Budget overflow " +
            std::to_string(index));
        object->transform.position = {
            static_cast<float>(index % 10u) - 5.f,
            2.f, static_cast<float>(index / 10u) };
        auto* light = object->AddComponent<Engine::Components::Light>();
        light->intensity = 0.5f + static_cast<float>(index) * 0.01f;
        light->range = 20.f;
    }
    Engine::Model::RealtimeShadowSettings settings{};
    settings.directionalResolution = 1024u;
    settings.directionalCascadeCount = 4u;
    scene.SetRealtimeShadowSettings(settings);
    scene.Start();
    uint64_t allocationGeneration = 0u;
    std::string selectedLight;
    double worstSubmitMs = 0.0;
    std::vector<uint8_t> pixels;
    for (uint32_t frame = 0; frame < 40u; ++frame)
    {
        double submitMs = 0.0;
        CHECK(RenderCapture(scene, renderer, pixels, &submitMs));
        const auto& debug = scene.GetShadowDebugSnapshot();
        CHECK(scene.GetLastRealtimeLightCount() ==
            Engine::Model::MaxRealtimeLights);
        CHECK(debug.hasSelectedLight);
        if (frame == 0u)
        {
            allocationGeneration = debug.atlasAllocationGeneration;
            selectedLight = debug.selectedLight;
        }
        CHECK(debug.atlasAllocationGeneration == allocationGeneration);
        CHECK(debug.selectedLight == selectedLight);
        if (frame >= 4u)
            worstSubmitMs = (std::max)(worstSubmitMs, submitMs);
    }
    // A multi-second submission would indicate an accidental per-frame wait.
    CHECK(worstSubmitMs < 2000.0);
    return true;
}
}

int main()
{
    try
    {
        if (!RunSpotAndPhysicalLightRegression() || !RunLightBudgetStress())
            return 1;
        Engine::Model::ProjectSettings project{};
        project.gameRenderingAPI = "DirectX11";
        std::string unavailableReason;
        if (!Engine::Renderers::RendererFactory::IsRendererAvailable(
            "DirectX11", &unavailableReason))
        {
            std::printf("shadow GPU regressions skipped: %s\n",
                unavailableReason.c_str());
            return 0;
        }
        Engine::Core::Window window(GetModuleHandleW(nullptr),
            L"Shadow Regression", Width, Height);
        auto rendererBase = Engine::Renderers::RendererFactory::
            CreateGameRenderer(project);
        auto* renderer = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(
            rendererBase.get());
        if (!renderer || !renderer->Init(window.GetHWND(), Width, Height))
        {
            std::printf("shadow GPU regressions skipped: DX11 initialization failed\n");
            return 0;
        }
        if (!renderer->EnableOffscreenExport(3u))
            throw std::runtime_error("DX11 offscreen readback initialization failed");
        if (!RunVisualRegressions(*renderer) ||
            !RunRenderedBudgetStress(*renderer))
            return 1;
        renderer->WaitIdle();
        std::printf("shadow regressions passed: bias seams shimmer fade thin "
            "reversed large-coordinates portals spatial-mapping max-lights "
            "determinism no-churn\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "shadow regression exception: %s\n", error.what());
        return 1;
    }
}
