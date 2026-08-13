#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/component.h"
#include "Core/Script.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Compoonents/Light.h"
#include "Core/Compoonents/Sprite.h"
#include "Core/Compoonents/AudioSource.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/Animation.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include <pugixml.hpp>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace Engine::Serialization
{
// ---- Registry ---------------------------------------------------------------
std::unordered_map<std::string, SceneSerializer::Factory>& SceneSerializer::GetRegistry()
{
    static std::unordered_map<std::string, Factory> s_registry;
    return s_registry;
}

void SceneSerializer::Register(const std::string& typeName, Factory factory)
{
    GetRegistry()[typeName] = std::move(factory);
}

void SceneSerializer::Unregister(const std::string& typeName)
{
    GetRegistry().erase(typeName);
}

SceneSerializer::Factory SceneSerializer::GetRegisteredFactory(
    const std::string& typeName)
{
    auto found = GetRegistry().find(typeName);
    return found == GetRegistry().end() ? Factory{} : found->second;
}

void SceneSerializer::Register(Factory factory)
{
    if (!factory)
        throw std::invalid_argument("Cannot register an empty component factory");

    std::unique_ptr<Engine::Core::Component> prototype(factory());
    if (!prototype || prototype->GetTypeName().empty())
        throw std::invalid_argument("Registered components must provide a type name");

    const std::string typeName = prototype->GetTypeName();
    Register(typeName, std::move(factory));
}

void SceneSerializer::EnsureBuiltinsRegistered()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    RegisterComponentType<Engine::Components::Mesh>();
    RegisterComponentType<Engine::Components::Material>();
    RegisterComponentType<Engine::Components::Camera>();
    RegisterComponentType<Engine::Components::Light>();
    RegisterComponentType<Engine::Components::Sprite>();
    RegisterComponentType<Engine::Components::AudioSource>();
    RegisterComponentType<Engine::Components::RigidBody>();
    RegisterComponentType<Engine::Components::PrimitiveObjectCollider>();
    RegisterComponentType<Engine::Components::MeshObjectCollider>();
    RegisterComponentType<Engine::Components::Cloth>();
    RegisterComponentType<Engine::Components::Canvas>();
    RegisterComponentType<Engine::Components::UIObject>();
    RegisterComponentType<Engine::Components::UIText>();
    RegisterComponentType<Engine::Components::SpriteAnimationManager>();
    RegisterComponentType<Engine::Components::Model>();
    RegisterComponentType<Engine::Components::Animation>();
    RegisterComponentType<Engine::Components::AnimationManager>();
    RegisterComponentType<Engine::Components::Skeleton>();
    RegisterComponentType<Engine::Components::SkinnedMesh>();
    RegisterComponentType<Engine::Rendering::BakedLightingData>();
}

Engine::Core::Component* SceneSerializer::CreateRegisteredComponent(const std::string& typeName)
{
    EnsureBuiltinsRegistered();
    auto& registry = GetRegistry();
    auto found = registry.find(typeName);
    if (found == registry.end())
    {
        found = std::find_if(registry.begin(), registry.end(), [&typeName](const auto& entry)
        {
            return entry.first.size() == typeName.size() &&
                std::equal(entry.first.begin(), entry.first.end(), typeName.begin(),
                    [](unsigned char left, unsigned char right)
                    {
                        return std::tolower(left) == std::tolower(right);
                    });
        });
    }
    return found != registry.end() && found->second ? found->second() : nullptr;
}

std::vector<std::string> SceneSerializer::GetRegisteredComponentTypes()
{
    EnsureBuiltinsRegistered();
    std::vector<std::string> types;
    types.reserve(GetRegistry().size());
    for (const auto& entry : GetRegistry())
    {
        std::unique_ptr<Engine::Core::Component> prototype(entry.second ? entry.second() : nullptr);
        if (prototype && prototype->editorAddable)
            types.push_back(entry.first);
    }
    std::sort(types.begin(), types.end(), [](const std::string& left, const std::string& right)
    {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) < std::tolower(b); });
    });
    return types;
}

std::vector<std::string> SceneSerializer::GetRegisteredScriptTypes()
{
    EnsureBuiltinsRegistered();
    std::vector<std::string> types;
    for (const auto& entry : GetRegistry())
    {
        std::unique_ptr<Engine::Core::Component> prototype(entry.second ? entry.second() : nullptr);
        if (prototype && dynamic_cast<Engine::Core::Script*>(prototype.get()))
            types.push_back(entry.first);
    }
    std::sort(types.begin(), types.end());
    return types;
}

}
