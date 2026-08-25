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

    // Top-level objects
    const JsonValue& objects = root["objects"];
    for (std::size_t i = 0; i < objects.ArraySize(); ++i)
    {
        if (!Detail::SceneObjectBehavior::DeserializeObjectTree(
            scene,
            objects.ArrayAt(i),
            graphicsProvider,
            registry))
            return false;
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

        Detail::SceneObjectBehavior::DeserializeOptions options;
        options.assignDefaultNameIfMissing = true;
        rootObject = Detail::SceneObjectBehavior::DeserializeObjectTree(
            scene,
            root["object"],
            graphicsProvider,
            GetRegistry(),
            options);
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
