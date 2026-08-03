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
    if (!cube || !cube->GetComponent<Rotate>())
    {
        std::cerr << "Default Cube does not contain Rotate\n";
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

    return 0;
}
