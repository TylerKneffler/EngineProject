#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Serialization/PreparedPrefab.h"
#include <future>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void CheckDocumentCache()
{
    namespace fs = std::filesystem;
    const auto directory = fs::path(".local/tests-generated") /
        ("prefab-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto path = directory / "cache.prefab";
    const auto write = [&](const char* name)
    {
        std::ofstream output(path);
        output << "<Prefab version=\"1\" type=\"prefab\"><object name=\"" << name
            << "\" enabled=\"true\"><components/><children/></object></Prefab>";
    };
    write("Cache first");
    Engine::Scene::Scene scene;
    auto* first = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, path.string());
    Check(first && first->name == "Cache first", "initial relative load failed");
    auto* second = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, fs::absolute(path).string());
    Check(second && second != first && second->name == first->name, "absolute/relative instance mismatch");
    Check(second->PrefabSourceSnapshot == first->PrefabSourceSnapshot, "prefab baselines differ");
    first->name = "Independent instance";
    Check(second->name == "Cache first", "instances share mutable state");
    const auto timestamp = fs::last_write_time(path);
    write("Cache edited and reloaded");
    fs::last_write_time(path, timestamp + std::chrono::seconds(2));
    auto* third = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, path.string());
    Check(third && third->name == "Cache edited and reloaded", "edited prefab was not invalidated");
    Check(second->name == "Cache first", "file reload changed an existing instance");
    fs::remove(path);
    fs::remove(directory);
    std::cout << "Prefab path identity, independent instances, and file invalidation passed" << std::endl;
}
}

int main(int argc, char** argv) {
    try {
    if (argc > 1 && std::string(argv[1]) == "--cache-test")
    { CheckDocumentCache(); return 0; }
    const bool asynchronous = argc > 1 && std::string(argv[1]) == "--async";
    const int pathArgument = asynchronous ? 2 : 1;
    const std::string path = argc > pathArgument ? argv[pathArgument] : "Engine/Core/Assets/Models/mmd/Mococo/Mococo Prefab/Mococo Abyssgard.low.prefab";
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Prefab placement profile", 256, 256);
    ShowWindow(window.GetHWND(), SW_HIDE);
    Engine::Renderers::DX12GameRenderer renderer;
    const auto initStart = std::chrono::steady_clock::now();
    if (!renderer.Init(window.GetHWND(),256,256)) return 2;
    std::cout << "renderer_init_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-initStart).count() << std::endl;
    auto* provider = renderer.GetGraphicsProvider();
    Engine::Scene::Scene scene;
    const auto sceneStart = std::chrono::steady_clock::now();
    scene.Init(provider);
    std::cout << "scene_init_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sceneStart).count() << std::endl;
    for (int iteration=0; iteration<2; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        Engine::Core::Object* root = nullptr;
        if (asynchronous)
        {
            auto canceled = std::make_shared<std::atomic_bool>(false);
            auto future = std::async(std::launch::async, [=] {
                return Engine::Serialization::PreparedPrefab::Load(path, canceled);
            });
            size_t ticks = 0;
            while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                ++ticks;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto prepared = future.get();
            const auto decoded = std::chrono::steady_clock::now();
            if (!prepared) throw std::runtime_error("Prefab preparation canceled");
            Engine::Serialization::PreparedPrefab::Publication publication(*prepared);
            root = Engine::Serialization::SceneSerializer::InstantiatePrefabFromValue(
                scene, path, prepared->document, provider);
            scene.QueueGraphicsPreparation(2.0);
            std::cout << "background_decode_ms=" << std::chrono::duration<double,std::milli>(decoded-started).count()
                << " owner_publish_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decoded).count()
                << " worker_wait_ticks=" << ticks << std::endl;
        }
        else root = Engine::Serialization::SceneSerializer::InstantiatePrefab(scene, path, provider);
        if (!root) { std::cerr << "Failed to instantiate " << path << std::endl; return 1; }
        const auto loaded = std::chrono::steady_clock::now();
        if (asynchronous)
        {
            double longest = 0;
            size_t frames = 0;
            while (scene.HasPendingGraphicsPreparation())
            {
                const auto step = std::chrono::steady_clock::now();
                scene.ProcessGraphicsPreparation(provider);
                longest = std::max(longest, std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-step).count());
                ++frames;
            }
            std::cout << "upload_batches=" << frames << " longest_upload_batch_ms=" << longest << std::endl;
        }
        scene.PrepareRenderFrame();
        renderer.WaitIdle();
        const auto prepared = std::chrono::steady_clock::now();
        std::cout << "load_ms=" << std::chrono::duration<double,std::milli>(loaded-started).count()
            << " graphics_prepare_ms=" << std::chrono::duration<double,std::milli>(prepared-loaded).count() << std::endl;
        size_t vertices=0, morphs=0;
        for (auto* mesh : scene.GetComponents<Engine::Components::Mesh>()) {
            vertices += mesh->GetVertices().size(); morphs += mesh->GetMorphTargetCount();
        }
        std::cout << "iteration=" << iteration << " total_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()
            << " objects=" << scene.GetObjects().size() << " vertices=" << vertices << " morphs=" << morphs << std::endl;
    }
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
