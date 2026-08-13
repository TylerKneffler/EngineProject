#include "Core/Serialization/SceneSerializerInternal.h"
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
// ---- Save -------------------------------------------------------------------
std::string SceneSerializer::SaveToString(const Engine::Scene::Scene& scene)
{
    return Detail::SceneXmlBehavior::WriteDocument(Detail::SceneObjectBehavior::SerializeScene(scene));
}

bool SceneSerializer::Save(const Engine::Scene::Scene& scene, const std::string& path)
{
    std::ofstream file(path);
    if (!file) return false;
    file << SaveToString(scene);
    return file.good();
}

bool Detail::SceneDocumentBehavior::DeserializeScene(Engine::Scene::Scene& scene, const JsonValue& root,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry)
{
    if (!root.IsObject()) return false;

    int version = root["version"].AsInt();
    if (version != 1) return false;

    // Clear existing scene content
    scene.ClearObjects();

    Engine::Scene::DeserializeSceneSettings(scene.settings, root["settings"]);

    // Deserialise a single Engine::Core::Object node, then recurse for children.
    // Uses std::function so it can reference itself (recursive lambda).
    std::function<void(Engine::Core::Object&, const JsonValue&)> deserialise
        = [&](Engine::Core::Object& obj, const JsonValue& node)
    {
        if (node.Has("name"))
            obj.name = node["name"].AsString();
        obj.enabled = !node.Has("enabled") || node["enabled"].AsBool();
        if (node.Has("prefab"))
            obj.SetPrefab(node["prefab"].AsString());

        // Engine::Components::Transform
        Detail::SceneObjectBehavior::DeserializeTransform(obj.transform, node["transform"]);

        // Components
        const JsonValue& comps = node["components"];
        for (std::size_t i = 0; i < comps.ArraySize(); ++i)
        {
            const JsonValue& cn  = comps.ArrayAt(i);
            const std::string& t = cn["type"].AsString();

            const SceneSerializer::Factory* factory = Detail::ComponentRegistryBehavior::FindFactory(registry, t);
            if (!factory)
            {
                std::string msg = "[SceneSerializer] Unknown component type '" + t + "' on object '" + obj.name + "' (skipped)\n";
                OutputDebugStringA(msg.c_str());
                continue;
            }

            Engine::Core::Component* comp = (*factory)();        // factory: default-construct
            comp->Owner = &obj;
            comp->Deserialize(cn);
            comp->OnAfterDeserialize(graphicsProvider);
            obj.Components.push_back(comp);
        }

        // Children (recursive) — added to scene so they participate in loops.
        const JsonValue& children = node["children"];
        for (std::size_t i = 0; i < children.ArraySize(); ++i)
        {
            const JsonValue& childNode = children.ArrayAt(i);
            Engine::Core::Object* child = nullptr;
            if (childNode.Has("prefab"))
            {
                child = SceneSerializer::InstantiatePrefab(
                    scene, childNode["prefab"].AsString(), graphicsProvider);
                if (!child)
                    throw std::runtime_error(
                        "Could not instantiate nested prefab '" +
                        childNode["prefab"].AsString() + "'");
                Detail::SceneObjectBehavior::DeserializeTransform(child->transform, childNode["transform"]);
                if (childNode.Has("enabled"))
                    child->enabled = childNode["enabled"].AsBool();
                if (childNode.Has("prefabOverrides"))
                    Detail::SceneObjectBehavior::ApplyPrefabPatches(*child, childNode["prefabOverrides"],
                        graphicsProvider);
                Detail::SceneObjectBehavior::ApplyBakedMaterialOverrides(
                    *child, childNode, graphicsProvider);
            }
            else
            {
                child = scene.AddObject();
                deserialise(*child, childNode);
            }
            child->Parent = &obj;
            obj.Children.push_back(child);
        }
    };

    // Top-level objects
    const JsonValue& objects = root["objects"];
    for (std::size_t i = 0; i < objects.ArraySize(); ++i)
    {
        const JsonValue& objectNode = objects.ArrayAt(i);
        if (objectNode.Has("prefab"))
        {
            Engine::Core::Object* object = SceneSerializer::InstantiatePrefab(
                scene, objectNode["prefab"].AsString(), graphicsProvider);
            if (!object)
                throw std::runtime_error(
                    "Could not instantiate prefab '" +
                    objectNode["prefab"].AsString() + "'");
            Detail::SceneObjectBehavior::DeserializeTransform(object->transform, objectNode["transform"]);
            if (objectNode.Has("enabled"))
                object->enabled = objectNode["enabled"].AsBool();
            if (objectNode.Has("prefabOverrides"))
                Detail::SceneObjectBehavior::ApplyPrefabPatches(*object, objectNode["prefabOverrides"],
                    graphicsProvider);
            Detail::SceneObjectBehavior::ApplyBakedMaterialOverrides(
                *object, objectNode, graphicsProvider);
        }
        else
        {
            Engine::Core::Object* object = scene.AddObject();
            deserialise(*object, objectNode);
        }
    }

    return true;
}

// ---- Load -------------------------------------------------------------------
bool SceneSerializer::LoadFromString(Engine::Scene::Scene& scene, const std::string& source,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try
    {
        if (Detail::SceneDocumentBehavior::DeserializeScene(
            scene, JsonParse(source), graphicsProvider, GetRegistry()))
            return true;
    }
    catch (const std::exception&) {}

    try
    {
        if (Detail::SceneXmlBehavior::LoadDocument(scene, source, graphicsProvider, GetRegistry(), false))
            return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed to load scene data: ") +
            error.what() + "\n").c_str());
        return false;
    }

    OutputDebugStringA("[SceneSerializer] Failed to load scene data: unsupported format\n");
    return false;
}

bool SceneSerializer::Load(Engine::Scene::Scene& scene, const std::string& path, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try { return Detail::SceneXmlBehavior::LoadDocument(scene, path, graphicsProvider, GetRegistry(), true); }
    catch (const std::exception&) {}

    try
    {
        return Detail::SceneDocumentBehavior::DeserializeScene(
            scene, JsonParseFile(path), graphicsProvider, GetRegistry());
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed to load '") + path +
            "': " + error.what() + "\n").c_str());
        return false;
    }
}

std::string SceneSerializer::SaveObjectToString(const Engine::Core::Object& object)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("object-clipboard")));
    root.Set("object", Detail::SceneObjectBehavior::SerializeObject(object, true));
    return JsonWrite(root);
}

Engine::Core::Object* SceneSerializer::InstantiateObjectFromString(
    Engine::Scene::Scene& scene, const std::string& source, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    Engine::Core::Object* rootObject = nullptr;
    try
    {
        const JsonValue root = JsonParse(source);
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            root["type"].AsString() != "object-clipboard" ||
            !root["object"].IsObject())
            return nullptr;

        std::function<Engine::Core::Object*(const JsonValue&)> instantiate;
        instantiate = [&](const JsonValue& node) -> Engine::Core::Object*
        {
            Engine::Core::Object* object = nullptr;
            if (node.Has("prefab"))
            {
                object = InstantiatePrefab(
                    scene, node["prefab"].AsString(), graphicsProvider);
                if (!object)
                    throw std::runtime_error(
                        "Could not instantiate copied prefab '" +
                        node["prefab"].AsString() + "'");
                Detail::SceneObjectBehavior::DeserializeTransform(object->transform, node["transform"]);
                if (node.Has("enabled"))
                    object->enabled = node["enabled"].AsBool();
                if (node.Has("prefabOverrides"))
                    Detail::SceneObjectBehavior::ApplyPrefabPatches(*object, node["prefabOverrides"],
                        graphicsProvider);
                if (!rootObject)
                    rootObject = object;
                return object;
            }

            object = scene.AddObject();
            if (!rootObject)
                rootObject = object;
            object->name = node.Has("name")
                ? node["name"].AsString() : std::string("Object");
            object->enabled = !node.Has("enabled") || node["enabled"].AsBool();
            Detail::SceneObjectBehavior::DeserializeTransform(object->transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t index = 0; index < components.ArraySize(); ++index)
            {
                const JsonValue& componentNode = components.ArrayAt(index);
                const SceneSerializer::Factory* factory = Detail::ComponentRegistryBehavior::FindFactory(
                    GetRegistry(), componentNode["type"].AsString());
                if (!factory)
                    continue;
                Engine::Core::Component* component = (*factory)();
                component->Owner = object;
                component->Deserialize(componentNode);
                component->OnAfterDeserialize(graphicsProvider);
                object->Components.push_back(component);
            }

            const JsonValue& children = node["children"];
            for (std::size_t index = 0; index < children.ArraySize(); ++index)
            {
                Engine::Core::Object* child = instantiate(children.ArrayAt(index));
                if (!child)
                    throw std::runtime_error("Could not instantiate copied child object");
                child->Parent = object;
                object->Children.push_back(child);
            }
            return object;
        };

        rootObject = instantiate(root["object"]);
        return rootObject;
    }
    catch (const std::exception& error)
    {
        if (rootObject)
            scene.RemoveObject(rootObject);
        OutputDebugStringA((std::string(
            "[SceneSerializer] Failed to instantiate copied object: ") +
            error.what() + "\n").c_str());
        return nullptr;
    }
}

}
