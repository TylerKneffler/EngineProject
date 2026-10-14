#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/PmxRigController.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Object.h"
#include "Core/Model/ProjectSettings.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include <Windows.h>
#include <Psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#pragma comment(lib, "Psapi.lib")

static void Checkpoint(const char* text)
{
    PROCESS_MEMORY_COUNTERS_EX info{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info));
    std::cout << text << ": working " << info.WorkingSetSize / (1024 * 1024)
              << " MiB, private " << info.PrivateUsage / (1024 * 1024) << " MiB\n" << std::flush;
}

static void SaveBmp(const std::vector<uint8_t>& rgba, unsigned width, unsigned height, const char* path)
{
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto* p = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
            const uint8_t bgra[] = { p[2], p[1], p[0], p[3] };
            out.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static uint64_t CheckMorphBuffers(Engine::Core::Object* object)
{
    uint64_t staticBytes = 0;
    if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
        if (auto* deltas = mesh->GetMorphDeltaBuffer())
        {
            if (deltas->GetUploadShadowSize() != 0 ||
                deltas->GetAccessMode() != Engine::Graphics::IGraphicsBuffer::AccessMode::Default)
                throw std::runtime_error("Static DX11 morph deltas retain an upload shadow");
            auto* weights = mesh->GetMorphWeightBuffer();
            if (!weights || weights->GetAccessMode() != Engine::Graphics::IGraphicsBuffer::AccessMode::Upload)
                throw std::runtime_error("Animated morph weights are not writable");
            staticBytes += deltas->GetSize();
        }
    return staticBytes;
}

int main()
{
    try
    {
        constexpr unsigned width = 960, height = 540;
        Engine::Core::Window window(GetModuleHandleW(nullptr), L"MMD hidden playback test", width, height);
        Engine::Model::ProjectSettings settings;
        settings.gameRenderingAPI = "DirectX11";
        auto renderer = Engine::Renderers::RendererFactory::CreateGameRenderer(settings);
        if (!renderer || !renderer->Init(window.GetHWND(), width, height)) throw std::runtime_error("DX11 initialization failed");
        auto* dx = dynamic_cast<Engine::Renderers::DX11GameRenderer*>(renderer.get());
        if (!dx || !dx->EnableOffscreenExport(width, height, 1, 2)) throw std::runtime_error("Offscreen capture initialization failed");
        Engine::Scene::Scene scene;
        Checkpoint("Before scene initialization");
        scene.Init(renderer->GetGraphicsProvider());
        Checkpoint("Before scene load");
        const char* selectedScene = std::getenv("MMD_SCENE");
        if (!scene.Load(selectedScene ? selectedScene :
            "Engine/Core/Assets/Scenes/Animation/mmd.scene"))
            throw std::runtime_error("Scene load failed");
        Checkpoint("After scene load");
        size_t dynamicBodies = 0;
        size_t physicsJoints = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (auto* body = object->GetComponent<Engine::Components::RigidBody>())
                dynamicBodies += body->bodyType == "Dynamic";
            if (auto* rig = object->GetComponent<Engine::Components::PmxRigController>())
                physicsJoints += rig->physicsJoints.size();
        }
        if (dynamicBodies != 133 || physicsJoints != 133)
            throw std::runtime_error("Mococo scene lost its PMX dynamic bodies or joints");
        size_t texturedMaterials = 0;
        for (const auto& object : scene.GetObjects())
            if (auto* material = object->GetComponent<Engine::Components::Material>())
                if (material->baseColorTexture)
                {
                    if (!std::filesystem::is_regular_file(
                        material->baseColorTexture->GetFilePath()))
                        throw std::runtime_error("Model material has a missing texture");
                    ++texturedMaterials;
                }
        if (texturedMaterials == 0)
            throw std::runtime_error("Model has no textured materials");
        uint64_t staticMorphBytes = 0;
        for (const auto& object : scene.GetObjects()) staticMorphBytes += CheckMorphBuffers(object.get());
        if (!staticMorphBytes) throw std::runtime_error("MMD scene has no GPU morph buffers");
        std::cout << "Static morph upload shadows avoided: " << staticMorphBytes / (1024 * 1024) << " MiB\n";
        const auto snapshot = scene.SaveToString();
        Checkpoint("After editor Play snapshot");
        scene.Start();
        Checkpoint("After Play start");
        std::filesystem::create_directories(".local/test-output/mmd-render");
        const auto render = [&]()
        {
            scene.PrepareRenderFrame();
            dx->BeginExportFrame(1);
            renderer->BeginFrame();
            auto context = renderer->CreateFrameGraphicsContext();
            auto* camera = scene.FindGameCamera();
            if (!context || !camera) throw std::runtime_error("Missing graphics context or camera");
            scene.Render(context.get(), float(width) / height, camera, false, width, height, true);
            renderer->Clear(.025f, .025f, .035f);
            scene.Render(context.get(), float(width) / height, camera, false, width, height, false);
            renderer->EndFrame();
            renderer->WaitIdle();
        };
        if (std::getenv("MMD_RENDER_VISUAL_AUDIT"))
        {
            bool flipped = false;
            for (int side : {1, -1})
                for (bool flip : {false, true})
                {
                    if (flip != flipped)
                    {
                        for (const auto& object : scene.GetObjects())
                            if (auto* mesh = object->GetComponent<Engine::Components::Mesh>())
                            {
                                auto vertices = mesh->GetVertices();
                                for (auto& vertex : vertices)
                                {
                                    vertex.uv[1] = 1.f - vertex.uv[1];
                                    vertex.uv1[1] = 1.f - vertex.uv1[1];
                                    vertex.tangent[3] *= -1.f;
                                }
                                mesh->UpdateAuthoredVertices(std::move(vertices));
                            }
                        flipped = flip;
                    }
                    auto* camera = scene.FindGameCamera();
                    camera->Owner->transform.position.z = side * 2.7f;
                    render();
                    std::vector<uint8_t> rgba;
                    if (!dx->ReadExportFrameRGBA(rgba, true)) throw std::runtime_error("Audit capture failed");
                    const std::string path = std::string(".local/test-output/mmd-render/audit-") +
                        (side > 0 ? "positive-z" : "negative-z") + (flip ? "-flipped.bmp" : "-original.bmp");
                    SaveBmp(rgba, width, height, path.c_str());
                    std::cout << "Captured " << path << '\n' << std::flush;
                }
            return 0;
        }
        for (unsigned frame = 0; frame < 660; ++frame)
        {
            scene.Update(1.f / 60.f);
            if (frame != 0 && frame != 30 && frame != 89 && frame != 320 && frame != 644) continue;
            render();
            std::vector<uint8_t> rgba;
            if (!dx->ReadExportFrameRGBA(rgba, true) || rgba.size() != width * height * 4) throw std::runtime_error("Frame readback failed");
            const auto path = ".local/test-output/mmd-render/frame-" + std::to_string(frame) + ".bmp";
            SaveBmp(rgba, width, height, path.c_str());
            Checkpoint("Captured animated frame");
        }
        for (unsigned cycle = 0; cycle < 3; ++cycle)
        {
            if (!scene.LoadFromString(snapshot)) throw std::runtime_error("Play snapshot restore failed");
            scene.Start();
            for (unsigned frame = 0; frame < 6; ++frame)
            {
                scene.Update(1.f / 60.f);
                render();
                std::vector<uint8_t> rgba;
                if (!dx->ReadExportFrameRGBA(rgba, true) || rgba.size() != width * height * 4)
                    throw std::runtime_error("Repeated Play frame readback failed");
            }
            Checkpoint("Repeated Play and rendering after snapshot restore");
        }
        std::cout << "DirectX11 MMD playback and Play snapshot restore passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "MMD rendered Play failure: " << error.what() << '\n';
        return 1;
    }
}
