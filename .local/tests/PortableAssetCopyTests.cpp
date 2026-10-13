#include "Editor/Core/Importers/PortableAssetCopy.h"
#include "Core/Rendering/Sprites/SpriteAnimationAsset.h"
#include "Core/Rendering/Sprites/SpriteSheetAsset.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    Check(static_cast<bool>(output), "Could not write fixture");
}

int main()
{
    const fs::path working = fs::current_path() / ".local/test-output/portable-import";
    const fs::path originalDirectory = fs::current_path();
    try
    {
        fs::remove_all(working);
        const fs::path source = working / "source";
        Write(source / "geometry/model.obj",
            "mtllib ../materials/model.mtl ../materials/extra.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        Write(source / "materials/model.mtl",
            "newmtl Painted\nmap_Kd ../images/diffuse.png\n");
        Write(source / "materials/extra.mtl", "newmtl Extra\n");
        Write(source / "images/diffuse.png", "texture bytes");
        Write(source / "animation/walk.spriteanim",
            "{\"spriteSheet\":\"../sheets/walk.spritesheet\",\"animation\":\"Walk\"}");
        Write(source / "sheets/walk.spritesheet",
            "{\"image\":\"../images/idle.png\",\"animations\":{\"Walk\":{\"frames\":["
            "{\"x\":0,\"y\":0,\"width\":1,\"height\":1},"
            "{\"image\":\"../images/step.png\",\"x\":0,\"y\":0,\"width\":1,\"height\":1}]}}}");
        Write(source / "images/idle.png", "idle bytes");
        Write(source / "images/step.png", "step bytes");

        const fs::path assets = working / "project/Assets";
        fs::create_directories(assets);
        const fs::path obj = assets / "model.obj";
        const fs::path animation = assets / "walk.spriteanim";
        Engine::Editor::CopyPortableAsset(source / "geometry/model.obj", obj);
        Engine::Editor::CopyPortableAsset(source / "animation/walk.spriteanim", animation);

        Engine::Scene::Scene scene;
        auto* model = scene.AddObject("Model");
        auto* mesh = model->AddComponent<Engine::Components::Mesh>();
        mesh->LoadFromFile(obj.string());
        mesh->SetAuthoredFilePath("Assets/model.obj");
        auto* spriteObject = scene.AddObject("Walk");
        auto* manager = spriteObject->AddComponent<Engine::Components::SpriteAnimationManager>();
        auto* sprite = spriteObject->AddComponent<Engine::Components::Sprite>();
        sprite->SetAnimationManager(manager);
        Check(manager->LoadFromFile(animation.string()), "Source sprite animation did not load");
        manager->animationFile = "Assets/walk.spriteanim";
        Check(Engine::Serialization::SceneSerializer::SavePrefab(*model,
            (assets / "model.prefab").string()), "OBJ prefab save failed");
        Check(Engine::Serialization::SceneSerializer::SavePrefab(*spriteObject,
            (assets / "walk.prefab").string()), "Sprite prefab save failed");

        const fs::path relocated = working / "relocated";
        fs::rename(working / "project", relocated);
        fs::current_path(relocated);
        Engine::Components::Mesh loadedMesh;
        loadedMesh.LoadFromFile("Assets/model.obj");
        Check(loadedMesh.GetVertexCount() == 3, "Relocated OBJ geometry failed to load");
        const auto objText = [&]
        {
            std::ifstream input("Assets/model.obj");
            return std::string(std::istreambuf_iterator<char>(input), {});
        }();
        Check(objText.find("mtllib model_dependencies/model.mtl model_dependencies/extra.mtl") != std::string::npos,
            "OBJ material reference is not portable");
        const fs::path material = fs::path("Assets/model_dependencies/model.mtl");
        Check(fs::is_regular_file(material), "OBJ material was not copied");
        Check(fs::is_regular_file(material.parent_path() / "extra.mtl"),
            "Second OBJ material was not copied");
        std::ifstream materialInput(material);
        const std::string materialText(std::istreambuf_iterator<char>{materialInput}, {});
        Check(materialText.find("map_Kd diffuse.png") != std::string::npos,
            "Material texture reference is not portable");
        Check(fs::is_regular_file(material.parent_path() / "diffuse.png"),
            "OBJ texture was not copied");

        Engine::Rendering::SpriteAnimationAsset loadedAnimation;
        Check(loadedAnimation.Load("Assets/walk.spriteanim"),
            "Relocated sprite animation failed to load");
        Check(!fs::path(loadedAnimation.spriteSheetFile).is_absolute(),
            "Sprite sheet reference is absolute");
        Engine::Rendering::SpriteSheetAsset loadedSheet;
        Check(loadedSheet.Load(loadedAnimation.ResolveSpriteSheet()),
            "Relocated sprite sheet failed to load");
        const auto* walk = loadedSheet.FindAnimation("Walk");
        Check(walk && walk->frames.size() == 2, "Sprite frames were lost");
        for (const auto& frame : walk->frames)
            Check(fs::is_regular_file(frame.image), "Sprite frame image was not copied");

        Engine::Scene::Scene restored;
        Check(Engine::Serialization::SceneSerializer::InstantiatePrefab(restored,
            "Assets/model.prefab", nullptr) != nullptr, "Relocated OBJ prefab failed");
        Check(Engine::Serialization::SceneSerializer::InstantiatePrefab(restored,
            "Assets/walk.prefab", nullptr) != nullptr, "Relocated sprite prefab failed");
        fs::current_path(originalDirectory);
        std::cout << "Portable OBJ and sprite dependency round trips passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        fs::current_path(originalDirectory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
