#pragma once

#include <array>
#include <string_view>

namespace Engine::Core { class Object; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
struct PrimitiveObjectDefinition
{
    const char* name;
    const char* meshPath;
};

const std::array<PrimitiveObjectDefinition, 9>& PrimitiveObjectDefinitions();
Engine::Core::Object* CreatePrimitiveObject(
    Engine::Scene::Scene& scene, std::string_view primitiveName);
}
