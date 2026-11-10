#include "Core/Serialization/SceneSerializerInternal.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Settings/SceneSettingsSerialization.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/component.h"
#include "Core/Script.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include <pugixml.hpp>
#include <fstream>
#include <future>
#include <iterator>
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
    auto document = CaptureSceneDocument(scene);
    return std::async(std::launch::async, [document = std::move(document)] {
        return EncodeSceneFile(document);
    }).get();
}

std::string SceneSerializer::SaveSnapshotToString(const Engine::Scene::Scene& scene)
{
    auto document = CaptureSceneDocument(scene);
    return std::async(std::launch::async, [document = std::move(document)] {
        return EncodeSceneSnapshot(document);
    }).get();
}

JsonValue SceneSerializer::CaptureSceneDocument(const Engine::Scene::Scene& scene)
{
    scene.AssertOwnerThread();
    return Detail::SceneObjectBehavior::SerializeScene(scene);
}

std::string SceneSerializer::EncodeSceneSnapshot(const JsonValue& document)
{
    return AssetWriteBinary(document);
}

std::string SceneSerializer::EncodeSceneFile(const JsonValue& document)
{
    return Detail::SceneXmlBehavior::WriteDocument(document);
}

JsonValue SceneSerializer::ReadSceneFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not read scene: " + path);
    const std::string source(std::istreambuf_iterator<char>(file), {});
    if (file.bad()) throw std::runtime_error("Could not read scene: " + path);
    return ParseSceneText(source);
}

JsonValue SceneSerializer::ParseSceneText(const std::string& source)
{
    pugi::xml_document xml;
    if (xml.load_buffer(source.data(), source.size()))
        return Detail::SceneXmlBehavior::ReadNode(xml.document_element());
    return AssetParse(source);
}

bool SceneSerializer::Save(const Engine::Scene::Scene& scene, const std::string& path)
{
    auto document = CaptureSceneDocument(scene);
    return std::async(std::launch::async,
        [document = std::move(document), path] {
            const std::string xml = EncodeSceneFile(document);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
            file.flush();
            return file.good();
        }).get();
}

bool Detail::SceneDocumentBehavior::DeserializeScene(Engine::Scene::Scene& scene, const JsonValue& root,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry)
{
    if (!root.IsObject()) return false;

    int version = root["version"].AsInt();
    if (version != 1) return false;

    // Clear existing scene content
    {
    PrefabLoadProfile::Stage stage("restore_clear_objects");
    scene.ClearObjects();
    }

    // Older scene files predate per-scene dimensional mode. Reset this field
    // before reading so loading a legacy scene after a 2D scene cannot inherit
    // the previous document's mode.
    scene.settings.dimension = Engine::Model::SceneDimension::ThreeD;
    Engine::Scene::DeserializeSceneSettings(scene.settings, root["settings"]);
    scene.SetEditorMode2D(scene.settings.dimension ==
        Engine::Model::SceneDimension::TwoD);

    // Top-level objects
    PrefabLoadProfile::Stage stage("restore_deserialize_objects");
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

bool SceneSerializer::BeginIncrementalLoadFromValue(Scene& scene,
    const JsonValue& snapshot)
{
    EnsureBuiltinsRegistered();
    scene.AssertOwnerThread();
    if (!snapshot.IsObject() || snapshot["version"].AsInt() != 1)
        return false;
    try
    {
        scene.BeginIncrementalRestore();
        scene.ClearObjects();
        scene.settings.dimension = Engine::Model::SceneDimension::ThreeD;
        Engine::Scene::DeserializeSceneSettings(scene.settings,
            snapshot["settings"]);
        scene.SetEditorMode2D(scene.settings.dimension ==
            Engine::Model::SceneDimension::TwoD);
        return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed to begin incremental restore: ") +
            error.what() + "\n").c_str());
        return false;
    }
}

bool SceneSerializer::ContinueIncrementalLoadFromValue(Scene& scene,
    const JsonValue& snapshot, IncrementalLoadState& state,
    std::size_t maxObjectNodes, bool& complete)
{
    EnsureBuiltinsRegistered();
    scene.AssertOwnerThread();
    complete = false;
    if (!snapshot.IsObject() || snapshot["version"].AsInt() != 1)
        return false;
    try
    {
        const auto& objects = snapshot["objects"];
        if (state.nextTopLevelObject > objects.ArraySize() ||
            state.completedTopLevelObjects > objects.ArraySize())
            return false;
        const auto& registry = GetRegistry();
        const std::size_t budget = std::max<std::size_t>(1, maxObjectNodes);
        std::size_t restoredNodes = 0;
        while (restoredNodes < budget)
        {
            if (state.pendingNodes.empty())
            {
                if (state.topLevelObjectInProgress)
                {
                    ++state.completedTopLevelObjects;
                    state.topLevelObjectInProgress = false;
                }
                if (state.nextTopLevelObject >= objects.ArraySize())
                    break;
                state.pendingNodes.push_back({
                    &objects.ArrayAt(state.nextTopLevelObject++), nullptr });
                state.topLevelObjectInProgress = true;
            }

            const auto pending = state.pendingNodes.back();
            state.pendingNodes.pop_back();
            if (!pending.node)
                return false;
            Engine::Core::Object* object =
                Detail::SceneObjectBehavior::DeserializeObjectNode(
                    scene, *pending.node, scene.GetGraphicsProvider(), registry);
            if (!object)
                return false;
            if (pending.parent && !scene.SetParentLocal(object, pending.parent))
                throw std::runtime_error("Could not attach restored scene object");

            if (!pending.node->Has("prefab"))
            {
                const auto& children = (*pending.node)["children"];
                for (std::size_t i = children.ArraySize(); i > 0; --i)
                    state.pendingNodes.push_back({ &children.ArrayAt(i - 1), object });
            }
            ++restoredNodes;
        }

        if (state.pendingNodes.empty() && state.topLevelObjectInProgress)
        {
            ++state.completedTopLevelObjects;
            state.topLevelObjectInProgress = false;
        }
        complete = state.completedTopLevelObjects == objects.ArraySize() &&
            state.pendingNodes.empty();
        if (complete)
            scene.EndIncrementalRestore();
        return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed during incremental restore: ") +
            error.what() + "\n").c_str());
        return false;
    }
}

// ---- Load -------------------------------------------------------------------
bool SceneSerializer::LoadFromString(Engine::Scene::Scene& scene, const std::string& source,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try
    {
        const auto document = std::async(std::launch::async, [source] {
            return ParseSceneText(source);
        }).get();
        return LoadFromValue(scene, document, graphicsProvider);
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed to load scene data: ") +
            error.what() + "\n").c_str());
        return false;
    }
}

bool SceneSerializer::LoadFromValue(Engine::Scene::Scene& scene, const JsonValue& snapshot,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try
    {
        return Detail::SceneDocumentBehavior::DeserializeScene(
            scene, snapshot, graphicsProvider, GetRegistry());
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((std::string("[SceneSerializer] Failed to restore snapshot: ") +
            error.what() + "\n").c_str());
        return false;
    }
}

bool SceneSerializer::Load(Engine::Scene::Scene& scene, const std::string& path, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try
    {
        const auto document = std::async(std::launch::async, [path] {
            return ReadSceneFile(path);
        }).get();
        return LoadFromValue(scene, document, graphicsProvider);
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
