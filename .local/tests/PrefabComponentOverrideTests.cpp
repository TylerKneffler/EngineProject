#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

int main()
{
    namespace fs = std::filesystem;
    using Engine::Serialization::SceneSerializer;
    const fs::path prefabPath = fs::temp_directory_path() /
        ("prefab-component-overrides-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".prefab");
    struct Cleanup
    {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove(path, error); }
    } cleanup{prefabPath};
    Cleanup sceneCleanup{prefabPath.string() + ".scene"};

    try
    {
        Engine::Scene::Scene source;
        auto* original = source.AddObject("Prefab object");
        const auto capturedScene = SceneSerializer::CaptureSceneDocument(source);
        auto sceneXml = std::async(std::launch::async, [capturedScene] {
            return SceneSerializer::EncodeSceneFile(capturedScene);
        }).get();
        {
            std::ofstream sceneFile(sceneCleanup.path, std::ios::binary);
            sceneFile << sceneXml;
        }
        auto parsedScene = std::async(std::launch::async, [&] {
            return SceneSerializer::ReadSceneFile(sceneCleanup.path.string());
        }).get();
        Engine::Scene::Scene parsedSceneInstance;
        CHECK(SceneSerializer::LoadFromValue(parsedSceneInstance, parsedScene));
        CHECK(parsedSceneInstance.GetObjects().size() == 1);
        CHECK(parsedSceneInstance.GetObjects().front()->name == "Prefab object");
        auto savePrefab = SceneSerializer::PreparePrefabSave(*original, prefabPath.string());
        CHECK(std::async(std::launch::async, std::move(savePrefab)).get());

        auto parsedPrefab = std::async(std::launch::async, [&] {
            return SceneSerializer::ReadPrefabFile(prefabPath.string());
        }).get();

        Engine::Scene::Scene scene;
        auto* first = SceneSerializer::InstantiatePrefabFromValue(
            scene, prefabPath.string(), parsedPrefab);
        auto* second = SceneSerializer::InstantiatePrefab(scene, prefabPath.string());
        CHECK(first && second);

        first->AddComponent<Engine::Components::AudioSource>();
        first->InvalidatePrefabOverrideCache();
        CHECK(SceneSerializer::HasPrefabOverrides(*first, false));
        CHECK(SceneSerializer::RevertPrefabOverrides(*first));
        CHECK(!first->GetComponent<Engine::Components::AudioSource>());
        CHECK(!SceneSerializer::HasPrefabOverrides(*first, false));

        first->AddComponent<Engine::Components::AudioSource>();
        first->InvalidatePrefabOverrideCache();
        CHECK(SceneSerializer::ApplyPrefabOverridesToAsset(*first, false));
        CHECK(first->GetComponent<Engine::Components::AudioSource>());
        CHECK(second->GetComponent<Engine::Components::AudioSource>());
        CHECK(!SceneSerializer::HasPrefabOverrides(*first, false));
        CHECK(!SceneSerializer::HasPrefabOverrides(*second, false));

        second->AddComponent<Engine::Components::Sprite>();
        second->InvalidatePrefabOverrideCache();
        const std::string serialized = SceneSerializer::SaveToString(scene);
        Engine::Scene::Scene restored;
        CHECK(SceneSerializer::LoadFromString(restored, serialized));
        bool foundLocalAddition = false;
        for (const auto& object : restored.GetObjects())
            if (object->GetComponent<Engine::Components::Sprite>())
            {
                foundLocalAddition = true;
                CHECK(object->GetComponent<Engine::Components::AudioSource>());
            }
        CHECK(foundLocalAddition);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
