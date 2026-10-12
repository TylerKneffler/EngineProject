#include "Core/Compoonents/Materials/Material.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
#ifdef _WIN32
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Window.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

using Engine::Components::Material;

namespace
{
bool NearlyEqual(float a, float b)
{
    return std::abs(a - b) < 0.00001f;
}

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "subsurface material: %s\n", message);
    return condition;
}

#ifdef _WIN32
bool Capture(Engine::Scene::Scene& scene,
    Engine::Renderers::DX11GameRenderer& renderer,
    std::vector<uint8_t>& pixels)
{
    constexpr uint32_t width = 640, height = 360;
    scene.PrepareRenderFrame();
    renderer.BeginFrame();
    auto context = renderer.CreateFrameGraphicsContext();
    auto* camera = scene.FindGameCamera();
    if (!context || !camera)
        return false;
    scene.Render(context.get(), static_cast<float>(width) / height,
        camera, false, width, height, true);
    renderer.Clear(0.01f, 0.01f, 0.01f);
    scene.Render(context.get(), static_cast<float>(width) / height,
        camera, false, width, height, false);
    renderer.EndFrame();
    renderer.WaitIdle();
    return renderer.ReadExportFrameRGBA(pixels, true);
}

double MeanRgbDifference(const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    if (a.size() != b.size() || a.empty())
        return 0.0;
    double difference = 0.0;
    for (size_t index = 0; index < a.size(); index += 4u)
        for (int channel = 0; channel < 3; ++channel)
            difference += std::abs(static_cast<int>(a[index + channel]) -
                static_cast<int>(b[index + channel]));
    return difference / (static_cast<double>(a.size() / 4u) * 3.0);
}

bool RenderedComparison()
{
    std::string unavailable;
    if (!Engine::Renderers::RendererFactory::IsRendererAvailable(
            "DirectX11", &unavailable))
    {
        std::printf("SSS rendered comparison skipped: %s\n",
            unavailable.c_str());
        return true;
    }
    constexpr uint32_t width = 640, height = 360;
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Subsurface Regression", width, height);
    Engine::Model::ProjectSettings project{};
    project.gameRenderingAPI = "DirectX11";
    auto base = Engine::Renderers::RendererFactory::CreateGameRenderer(project);
    auto* renderer = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(base.get());
    if (!renderer || !renderer->Init(window.GetHWND(), width, height))
    {
        std::printf("SSS rendered comparison skipped: DX11 initialization failed\n");
        return true;
    }
    if (!Check(renderer->EnableOffscreenExport(width, height, 1u, 3u),
            "offscreen export initialization failed"))
        return false;
    Engine::Scene::Scene scene;
    scene.Init(renderer->GetGraphicsProvider());
    if (!Check(scene.Load(
            "Engine/Core/Assets/Scenes/Rendering/subsurface_scattering_demo.scene"),
            "rendered SSS scene failed to load"))
        return false;
    scene.Start();
    std::vector<uint8_t> enabled, disabled, changed;
    if (!Check(Capture(scene, *renderer, enabled),
            "SSS image capture failed"))
        return false;
    auto* broadObject = scene.FindObjectByName("SSS Broad Red Radius");
    auto* broad = broadObject ? broadObject->GetComponent<Material>() : nullptr;
    if (!Check(broad != nullptr, "broad-radius comparison is missing"))
        return false;
    const glm::vec3 broadRadius = broad->subsurfaceRadius;
    broad->subsurfaceRadius = { 0.004f, 0.002f, 0.001f };
    if (!Check(Capture(scene, *renderer, changed) &&
            MeanRgbDifference(enabled, changed) > 0.001,
            "changing RGB radius did not change the image"))
        return false;
    broad->subsurfaceRadius = broadRadius;
    auto* earObject = scene.FindObjectByName("Painted Thin Ear");
    auto* ear = earObject ? earObject->GetComponent<Material>() : nullptr;
    if (!Check(ear != nullptr, "painted-thickness comparison is missing"))
        return false;
    ear->SetSubsurfaceThicknessTexture({});
    if (!Check(Capture(scene, *renderer, changed) &&
            MeanRgbDifference(enabled, changed) > 0.001,
            "thickness map did not change the image"))
        return false;
    for (const auto& object : scene.GetObjects())
        if (object)
            if (auto* material = object->GetComponent<Material>())
                material->subsurfaceEnabled = false;
    if (!Check(Capture(scene, *renderer, disabled),
            "PBR control image capture failed"))
        return false;
    if (!Check(enabled.size() == disabled.size() && !enabled.empty(),
            "SSS image sizes differ"))
        return false;
    return Check(MeanRgbDifference(enabled, disabled) > 0.03,
        "SSS toggle did not visibly change the rendered scene");
}

bool BackendSmokeChecks()
{
    constexpr uint32_t width = 640, height = 360;
    for (const char* api : { "DirectX12", "Vulkan" })
    {
        std::string unavailable;
        if (!Engine::Renderers::RendererFactory::IsRendererAvailable(
                api, &unavailable))
        {
            std::printf("SSS %s smoke check skipped: %s\n", api,
                unavailable.c_str());
            continue;
        }
        Engine::Core::Window window(GetModuleHandleW(nullptr),
            L"Subsurface Backend Check", width, height);
        Engine::Model::ProjectSettings project{};
        project.gameRenderingAPI = api;
        auto renderer = Engine::Renderers::RendererFactory::
            CreateGameRenderer(project);
        if (!Check(renderer && renderer->Init(window.GetHWND(), width, height),
                "SSS backend initialization failed"))
            return false;
        Engine::Scene::Scene scene;
        try { scene.Init(renderer->GetGraphicsProvider()); }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "SSS backend scene init: %s\n", error.what());
            if (auto* dx12 = dynamic_cast<Engine::Renderers::DX12GameRenderer*>(renderer.get()))
            {
                Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
                if (SUCCEEDED(dx12->GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))))
                {
                    const auto count = queue->GetNumStoredMessages();
                    for (auto index = count > 8 ? count - 8 : 0; index < count; ++index)
                    {
                        SIZE_T size = 0;
                        queue->GetMessage(index, nullptr, &size);
                        std::vector<uint8_t> bytes(size);
                        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                        if (SUCCEEDED(queue->GetMessage(index, message, &size)))
                            std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
                    }
                }
            }
            return false;
        }
        if (!Check(scene.Load(
                "Engine/Core/Assets/Scenes/Rendering/subsurface_scattering_demo.scene"),
                "SSS backend scene load failed"))
            return false;
        scene.Start();
        const int resolutionCount = 2;
        for (int resolution = 0; resolution < resolutionCount; ++resolution)
        {
            const uint32_t renderWidth = resolution == 0 ? width : width / 2u;
            const uint32_t renderHeight = resolution == 0 ? height : height / 2u;
            if (resolution > 0)
                renderer->Resize(renderWidth, renderHeight);
            for (int frame = 0; frame < 4; ++frame)
            {
                scene.PrepareRenderFrame();
                renderer->BeginFrame();
                auto context = renderer->CreateFrameGraphicsContext();
                auto* camera = scene.FindGameCamera();
                if (!Check(context && camera,
                        "SSS backend context or camera missing"))
                    return false;
                scene.Render(context.get(),
                    static_cast<float>(renderWidth) / renderHeight,
                    camera, false, renderWidth, renderHeight, true);
                renderer->Clear(0.01f, 0.01f, 0.01f);
                scene.Render(context.get(),
                    static_cast<float>(renderWidth) / renderHeight,
                    camera, false, renderWidth, renderHeight, false);
                renderer->EndFrame();
                renderer->WaitIdle();
            }
            const auto timing = renderer->GetFrameTimingTelemetry();
            if (timing.gpuTimingsValid)
            {
                const double opaqueMilliseconds = timing.gpuMilliseconds[
                    static_cast<size_t>(Engine::Graphics::GpuTimingStage::Opaque)];
                if (!Check(std::isfinite(opaqueMilliseconds) &&
                        opaqueMilliseconds < 2000.0,
                        "SSS opaque GPU timing is invalid or stalled"))
                    return false;
                std::printf("SSS %s %ux%u opaque GPU: %.3f ms\n",
                    api, renderWidth, renderHeight, opaqueMilliseconds);
            }
            else
                std::printf("SSS %s %ux%u frame completed; GPU timing unavailable\n",
                    api, renderWidth, renderHeight);
        }
    }
    return true;
}
#endif
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
    const HRESULT shaderResult = D3DCompileFromFile(
        L"Engine/Core/Shaders/Object/Object.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", 0, 0,
        &shader, &errors);
    if (!Check(SUCCEEDED(shaderResult), errors
            ? static_cast<const char*>(errors->GetBufferPointer())
            : "DX11/DX12 object pixel shader failed to compile"))
        return 1;
#endif
    Material authored;
    authored.subsurfaceEnabled = true;
    authored.subsurfaceWeight = 0.83f;
    authored.subsurfaceRadius = { 0.08f, 0.025f, 0.009f };
    authored.subsurfaceColor = { 1.f, 0.42f, 0.25f };
    authored.subsurfaceThickness = 0.012f;
    authored.subsurfaceTransmission = 0.72f;
    authored.SetSubsurfaceThicknessTexture(
        "Engine/Core/Assets/Textures/sss_thickness_ear.png");

    Material sceneCopy;
    sceneCopy.Deserialize(authored.Serialize());
    if (!Check(sceneCopy.subsurfaceEnabled &&
            NearlyEqual(sceneCopy.subsurfaceWeight, 0.83f) &&
            NearlyEqual(sceneCopy.subsurfaceRadius.y, 0.025f) &&
            NearlyEqual(sceneCopy.subsurfaceColor.z, 0.25f) &&
            NearlyEqual(sceneCopy.subsurfaceThickness, 0.012f) &&
            NearlyEqual(sceneCopy.subsurfaceTransmission, 0.72f) &&
            sceneCopy.subsurfaceThicknessTexture != nullptr,
            "scene serialization lost subsurface controls"))
        return 1;

    const auto path = std::filesystem::path(".local/tests/subsurface_roundtrip.material");
    if (!Check(authored.SaveToFile(path.string()), "saving material failed"))
        return 1;
    Material assetCopy;
    const bool loaded = assetCopy.LoadFromFile(path.string());
    std::filesystem::remove(path);
    if (!Check(loaded && assetCopy.subsurfaceEnabled &&
            NearlyEqual(assetCopy.subsurfaceRadius.x, 0.08f) &&
            NearlyEqual(assetCopy.subsurfaceTransmission, 0.72f) &&
            assetCopy.subsurfaceThicknessTexture != nullptr,
            "material asset round trip lost subsurface controls"))
        return 1;

    Material fallback;
    if (!Check(!fallback.subsurfaceEnabled &&
            NearlyEqual(fallback.subsurfaceWeight, 0.f),
            "import fallback must leave subsurface disabled"))
        return 1;

    authored.subsurfaceWeight = 4.f;
    authored.subsurfaceRadius.x = -1.f;
    authored.Validate();
    if (!Check(NearlyEqual(authored.subsurfaceWeight, 1.f) &&
            NearlyEqual(authored.subsurfaceRadius.x, 0.f),
            "subsurface controls were not clamped"))
        return 1;

    Engine::Scene::Scene scene;
    if (!Check(Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Rendering/subsurface_scattering_demo.scene",
            nullptr),
            "SSS example scene failed to load"))
        return 1;
    int controls = 0;
    int scatteringMaterials = 0;
    int paintedThickness = 0;
    for (const auto& object : scene.GetObjects())
    {
        if (!object)
            continue;
        const auto* material = object->GetComponent<Material>();
        if (!material)
            continue;
        if (material->subsurfaceEnabled)
            ++scatteringMaterials;
        else
            ++controls;
        if (material->subsurfaceThicknessTexture)
            ++paintedThickness;
    }
    if (!Check(controls >= 1 && scatteringMaterials >= 3 &&
            paintedThickness == 1,
            "SSS example scene is missing a comparison or thickness map"))
        return 1;
#ifdef _WIN32
    if (!RenderedComparison() || !BackendSmokeChecks())
        return 1;
#endif
    return 0;
}
