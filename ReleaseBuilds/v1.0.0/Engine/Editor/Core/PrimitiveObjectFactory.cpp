#include "PrimitiveObjectFactory.h"

#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"

#include <stdexcept>

namespace Engine::Editor
{
const std::array<PrimitiveObjectDefinition, 9>& PrimitiveObjectDefinitions()
{
    static constexpr std::array<PrimitiveObjectDefinition, 9> definitions{{
        { "Cube",     "Assets/Mesh/cube.obj" },
        { "Quad",     "Assets/Mesh/quad.obj" },
        { "Plane",    "Assets/Mesh/plane.obj" },
        { "Sphere",   "Assets/Mesh/sphere.obj" },
        { "Cone",     "Assets/Mesh/cone.obj" },
        { "Pyramid",  "Assets/Mesh/pyramid.obj" },
        { "Cylinder", "Assets/Mesh/cylinder.obj" },
        { "Capsule",  "Assets/Mesh/capsule.obj" },
        { "Torus",    "Assets/Mesh/torus.obj" },
    }};
    return definitions;
}

Engine::Core::Object* CreatePrimitiveObject(
    Engine::Scene::Scene& scene, std::string_view primitiveName)
{
    const PrimitiveObjectDefinition* definition = nullptr;
    for (const PrimitiveObjectDefinition& candidate : PrimitiveObjectDefinitions())
    {
        if (primitiveName == candidate.name)
        {
            definition = &candidate;
            break;
        }
    }
    if (!definition)
        throw std::invalid_argument("Unknown primitive mesh: " +
            std::string(primitiveName));

    Engine::Core::Object* object = scene.AddObject(definition->name);
    try
    {
        Engine::Components::Mesh* mesh =
            object->AddComponent<Engine::Components::Mesh>();
        mesh->LoadFromFile(definition->meshPath);
        if (scene.GetGraphicsProvider())
            mesh->CreateBuffer(scene.GetGraphicsProvider()->GetBufferFactory());
        object->AddComponent<Engine::Components::Material>();
        return object;
    }
    catch (...)
    {
        scene.RemoveObject(object);
        throw;
    }
}
}
