#include "Core/Assets/Scripts/Rotate.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <cmath>
#include <filesystem>
#include <iostream>

int main()
{
    RegisterComponentType<Rotate>("Rotate");

    Scene registryScene;
    constexpr const char* registryProbe = R"({"version":1,"settings":{},"objects":[{"name":"Probe","enabled":true,"transform":{},"components":[{"type":"Rotate","axis":[0,1,0],"speed":45}],"children":[]}]})";
    if (!SceneSerializer::LoadFromString(registryScene, registryProbe, nullptr) ||
        registryScene.GetObjects().empty() ||
        !registryScene.GetObjects().front()->GetComponent<Rotate>())
    {
        std::cerr << "Rotate registry probe failed\n";
        return 4;
    }

    Scene scene;
    const std::filesystem::path scenePath =
        std::filesystem::path(ENGINE_ASSETS_PATH) / "Scenes" / "default.scene";
    if (!SceneSerializer::Load(scene, scenePath.string(), nullptr))
    {
        std::cerr << "Could not load default scene\n";
        return 1;
    }

    Object* cube = nullptr;
    for (const auto& object : scene.GetObjects())
        if (object->name == "Cube")
            cube = object.get();
    if (!cube || !cube->GetComponent<Rotate>() || !cube->Prefab ||
        cube->Prefab->GetPath() != "Assets/Prefabs/Cube.prefab")
    {
        std::cerr << "Default Cube prefab or Rotate component is missing\n";
        if (cube)
            for (const Component* component : cube->Components)
                std::cerr << "Loaded component: " << component->GetTypeName() << '\n';
        return 2;
    }

    const glm::vec3 before = cube->transform.rotation;
    cube->Start();
    cube->Update();
    if (glm::length(cube->transform.rotation - before) <= 0.000001f)
    {
        std::cerr << "Rotate did not change the Cube transform\n";
        return 3;
    }

    const std::string snapshot = scene.SaveToString();
    Scene::ObjectPath cubePath;
    if (!scene.TryGetObjectPath(cube, cubePath))
    {
        std::cerr << "Could not capture Cube hierarchy selection path\n";
        return 7;
    }
    if (!SceneSerializer::LoadFromString(scene, snapshot, nullptr))
    {
        std::cerr << "Could not restore play-mode snapshot\n";
        return 5;
    }
    cube = scene.FindObjectByPath(cubePath);
    if (!cube || !cube->GetComponent<Rotate>())
    {
        std::cerr << "Rotate was removed by play-mode snapshot restore\n";
        return 6;
    }

    Object* camera = nullptr;
    Object* light = nullptr;
    for (const auto& object : scene.GetObjects())
    {
        if (object->name == "Camera") camera = object.get();
        if (object->name == "Global Light") light = object.get();
    }
    if (!camera || !light)
        return 8;
    const glm::vec3 lightWorldBefore = light->transform.GetWorldPosition();
    if (!scene.MoveObject(light, cube, Scene::ObjectPlacement::AsChild) ||
        light->Parent != cube ||
        scene.MoveObject(cube, light, Scene::ObjectPlacement::AsChild))
    {
        std::cerr << "Hierarchy reparenting or cycle prevention failed\n";
        return 9;
    }
    if (glm::length(light->transform.GetWorldPosition() - lightWorldBefore) > 0.001f ||
        !scene.MoveObject(light, camera, Scene::ObjectPlacement::Before) ||
        light->Parent != nullptr)
    {
        std::cerr << "Hierarchy ordering or world transform preservation failed\n";
        return 10;
    }

    Object* copiedParent = scene.AddObject("Copy Parent");
    if (!scene.MoveObject(light, copiedParent, Scene::ObjectPlacement::AsChild))
        return 11;
    const std::string copiedGroup = SceneSerializer::SaveObjectToString(*copiedParent);
    Object* copiedCube = SceneSerializer::InstantiateObjectFromString(
        scene, copiedGroup, nullptr);
    if (!copiedCube || copiedCube == copiedParent ||
        copiedCube->Children.size() != 1 ||
        copiedCube->Children.front() == light ||
        copiedCube->Children.front()->name != light->name)
    {
        std::cerr << "Object hierarchy clipboard copy failed\n";
        return 12;
    }

    return 0;
}
