#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#endif
#include "InspectorTestUi.h"
#include <Windows.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

using Engine::Components::Material;
using Engine::Components::MaterialShader;
namespace fs = std::filesystem;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void PersistenceAndInspector()
{
    fs::create_directories(".local/artifacts/material-shaders");
    const fs::path directory = ".local/artifacts/material-shaders";
    std::ofstream(directory / "local.hlsl") << "// relative shader reference";
    Material source;
    source.shaders = {{"", true, "Auto"},
        {"Assets/Shaders/WarmOverlay.hlsl", true, "Alpha"},
        {"local.hlsl", false, "Additive"}};
    const auto json = source.Serialize();
    Require(json["shaders"].ArraySize() == 3, "Shader list not serialized");
    Require(!json.Has("shaderError"), "Runtime error serialized");
    Material restored;
    restored.Deserialize(json);
    Require(restored.shaders.size() == 3 && !restored.shaders[2].enabled &&
        restored.shaders[2].blendMode == "Additive", "Component round trip lost passes");
    Require(source.SaveToFile((directory / "test.material").string()), "Material save failed");
    Require(restored.LoadFromFile((directory / "test.material").string()), "Material load failed");
    Require(restored.shaders[1].path == source.shaders[1].path, "Shader path changed on load");
    Require(fs::equivalent(restored.ResolveShaderPath("local.hlsl"), directory / "local.hlsl"),
        "Material-relative shader path did not resolve");
    Require(fs::is_regular_file(restored.ResolveShaderPath("Assets/Shaders/WarmOverlay.hlsl")),
        "Bundled shader path did not resolve");
    auto overrideJson = json;
    overrideJson.Set("materialAsset", Engine::Serialization::JsonValue((directory / "test.material").string()));
    overrideJson.Set("shaders", Engine::Serialization::JsonValue::MakeArray());
    restored.Deserialize(overrideJson);
    Require(restored.shaders.empty(), "Empty component override did not clear asset shaders");
    std::ofstream(directory / "legacy.material") << "{\"baseColor\":[0,1,0]}";
    restored.shaders = source.shaders;
    Require(restored.LoadFromFile((directory / "legacy.material").string()) && restored.shaders.empty(),
        "Legacy material retained old shader stack");
    auto strings = Engine::Serialization::JsonValue::MakeObject();
    strings.Set("shaders", Engine::Serialization::JsonValue::MakeArray()
        .Push(Engine::Serialization::JsonValue("Assets/Shaders/WarmOverlay.hlsl")));
    restored.Deserialize(strings);
    Require(restored.shaders.size() == 1 && restored.shaders[0].enabled,
        "String path list was not accepted");
    restored.shaders[0].blendMode = "invalid";
    restored.Validate();
    Require(restored.shaders[0].blendMode == "Auto", "Invalid blend mode not normalized");

    Engine::Editor::InspectorTestUi ui;
    restored.shaders = source.shaders;
    ui.clickButton = "Add shader";
    Require(restored.DrawShaderProperties(ui) && restored.shaders.size() == 4,
        "Inspector cannot add to a three-entry shader list");
    ui.clickButton = "Move down";
    Require(restored.DrawShaderProperties(ui) && restored.shaders[0].path == source.shaders[1].path,
        "Inspector cannot reorder shader paths");
    ui.clickButton = "Remove";
    Require(restored.DrawShaderProperties(ui) && restored.shaders.size() == 3,
        "Inspector cannot remove shader paths");
    std::cout << "Shader persistence, relative paths, overrides and inspector: passed\n";
}

template<class Renderer>
static void RenderStack(const char* name, bool terrain)
{
    constexpr unsigned Size = 96;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Material shader regression", Size, Size);
    Renderer renderer;
    Require(renderer.Init(window.GetHWND(), Size, Size), "Renderer initialization failed");
    if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
        Require(renderer.EnableOffscreenExport(Size, Size, 1, 2), "Offscreen target failed");
    Engine::Scene::Scene scene;
    try { scene.Init(renderer.GetGraphicsProvider()); }
    catch (...)
    {
        if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX12GameRenderer>)
        {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
            if (SUCCEEDED(renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))))
                for (UINT64 index = 0; index < queue->GetNumStoredMessages(); ++index)
                {
                    SIZE_T size = 0;
                    queue->GetMessage(index, nullptr, &size);
                    std::vector<uint8_t> bytes(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                    if (SUCCEEDED(queue->GetMessage(index, message, &size)) &&
                        message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                        std::cerr << message->pDescription << '\n';
                }
        }
        throw;
    }
    auto* cameraObject = scene.AddObject("Camera");
    cameraObject->transform.position = {0.f, 0.f, -3.f};
    auto* camera = cameraObject->AddComponent<Engine::Components::Camera>();
    auto* object = scene.AddObject("Surface");
    auto* material = object->AddComponent<Material>();
    material->diffuseColor = {0.f, 1.f, 0.f};
    material->unlit = true;
    material->doubleSided = true;
    auto* mesh = object->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Components::Mesh::Vertex> vertices(3);
    vertices[0].pos[0] = -1.5f; vertices[0].pos[1] = -1.5f;
    vertices[1].pos[0] = 0.f; vertices[1].pos[1] = 1.5f;
    vertices[2].pos[0] = 1.5f; vertices[2].pos[1] = -1.5f;
    for (auto& vertex : vertices) { vertex.normal[2] = -1.f; vertex.tangent[0] = 1.f; vertex.tangent[3] = 1.f; }
    if (terrain)
        Require(mesh->SetTerrainGeometry(Engine::Components::Mesh::BuildIndexedTerrain(vertices)), "Terrain geometry failed");
    else
        Require(mesh->SetAuthoredVertices(std::move(vertices)), "Mesh geometry failed");
    mesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    const auto capture = [&]
    {
        if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
            renderer.BeginExportFrame(1);
        renderer.BeginFrame();
        renderer.Clear(0.f, 0.f, 0.f);
        auto context = renderer.CreateFrameGraphicsContext();
        scene.PrepareRenderFrame();
        scene.Render(context.get(), 1.f, camera, false, Size, Size);
        if constexpr (!std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
            renderer.RequestSceneCapture();
        renderer.EndFrame();
        renderer.WaitIdle();
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
            Require(renderer.ReadExportFrameRGBA(pixels, true), "Readback failed");
        else
            Require(renderer.ReadSceneCaptureRGBA(pixels), "Readback failed");
        const size_t center = ((Size / 2) * Size + Size / 2) * 4;
        return glm::ivec3(pixels[center], pixels[center + 1], pixels[center + 2]);
    };
    const auto captureUntil = [&](auto ready)
    {
        glm::ivec3 pixels{};
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            pixels = capture();
            if (ready(pixels)) return pixels;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pixels;
    };
    const auto base = capture();
    Require(base.g > base.r + 40, "Built-in surface is not green");
    const MaterialShader warm{"Assets/Shaders/WarmOverlay.hlsl", true, "Alpha"};
    material->shaders = {{"", true, "Auto"}, warm};
    const auto tinted = captureUntil([&](const glm::ivec3& pixels) {
        return pixels.r > base.r + 30 && pixels.g < base.g - 5;
    });
    std::cout << name << (terrain ? " terrain" : " mesh") << " center: base "
        << base.r << ',' << base.g << ',' << base.b << " overlay "
        << tinted.r << ',' << tinted.g << ',' << tinted.b << '\n';
    Require(material->shaderError.empty(), material->shaderError.c_str());
    Require(tinted.r > base.r + 30 && tinted.g < base.g - 5, "Overlay did not alpha blend");
    std::swap(material->shaders[0], material->shaders[1]);
    const auto reordered = captureUntil([&](const glm::ivec3& pixels) {
        return glm::all(glm::lessThanEqual(glm::abs(pixels - base), glm::ivec3(3)));
    });
    Require(glm::all(glm::lessThanEqual(glm::abs(reordered - base), glm::ivec3(3))),
        "Shader pass order did not affect compositing");
    material->shaders = {{"", true, "Auto"}, warm};
    material->shaders[1].enabled = false;
    Require(glm::all(glm::lessThanEqual(glm::abs(capture() - base), glm::ivec3(3))),
        "Disabled overlay still rendered");
    material->shaders = {{"Assets/Shaders/missing.hlsl", true, "Auto"}};
    Require(glm::all(glm::lessThanEqual(glm::abs(capture() - base), glm::ivec3(3))),
        "Missing surface did not fall back");
    Require(!material->shaderError.empty(), "Missing shader has no diagnostic");
    material->shaders = {{"", true, "Auto"}, {"Assets/not-a-shader.png", true, "Alpha"}};
    Require(glm::all(glm::lessThanEqual(glm::abs(capture() - base), glm::ivec3(3))),
        "Invalid overlay did not leave surface intact");
    Require(material->shaderError.find(".hlsl") != std::string::npos, "Wrong file type has no diagnostic");
    material->shaders = {{"", true, "Auto"}, warm};
    material->shaders[1].blendMode = "Additive";
    const auto additive = captureUntil([&](const glm::ivec3& pixels) {
        return pixels.r > base.r + 30 && pixels.g >= base.g - 3;
    });
    Require(additive.r > base.r + 30 && additive.g >= base.g - 3, "Additive pass did not add light");
    material->shaders = {warm};
    material->shaders[0].blendMode = "Opaque";
    const auto opaque = captureUntil([&](const glm::ivec3& pixels) {
        return pixels.r > tinted.r + 20 && pixels.g < tinted.g - 20;
    });
    Require(opaque.r > tinted.r + 20 && opaque.g < tinted.g - 20,
        "Custom surface did not replace the built-in shader");
    if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer> ||
        std::is_same_v<Renderer, Engine::Renderers::DX12GameRenderer>)
    {
        const fs::path broken = fs::path(".local/artifacts/material-shaders") /
            (std::string(name) + (terrain ? "-terrain" : "-mesh") + ".hlsl");
        std::ofstream(broken) << "invalid shader syntax";
        material->shaders = {{"", true, "Auto"}, {fs::absolute(broken).generic_string(), true, "Alpha"}};
        Require(glm::all(glm::lessThanEqual(glm::abs(capture() - base), glm::ivec3(3))),
            "Compile failure did not skip the overlay");
        captureUntil([&](const glm::ivec3&) { return !material->shaderError.empty(); });
        Require(!material->shaderError.empty(), "Compile failure has no diagnostic");
        std::ofstream(broken) << "#include \"" << fs::absolute(material->ResolveShaderPath(warm.path)).generic_string() << "\"\n";
        fs::last_write_time(broken, fs::file_time_type::clock::now() + std::chrono::seconds(1));
        const auto repaired = captureUntil([&](const glm::ivec3& pixels) {
            return material->shaderError.empty() && pixels.r > base.r + 30;
        });
        Require(material->shaderError.empty(), material->shaderError.c_str());
        Require(repaired.r > base.r + 30, "Source edit did not rebuild failed cached shader");
    }
    renderer.WaitIdle();
    std::cout << name << (terrain ? " terrain" : " mesh") << ": blend, order, disabled and fallback passed\n";
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    try
    {
        PersistenceAndInspector();
        const std::string api = argc > 1 ? argv[1] : "DirectX11";
        for (bool terrain : {false, true})
        {
            if (api == "DirectX11") RenderStack<Engine::Renderers::DX11GameRenderer>(api.c_str(), terrain);
            else if (api == "DirectX12") RenderStack<Engine::Renderers::DX12GameRenderer>(api.c_str(), terrain);
#if defined(ENGINE_VULKAN_ENABLED)
            else if (api == "Vulkan") RenderStack<Engine::Renderers::VulkanGameRenderer>(api.c_str(), terrain);
#endif
            else throw std::runtime_error("Unknown renderer argument");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
