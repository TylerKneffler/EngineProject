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
bool SceneSerializer::SavePrefab(const Engine::Core::Object& object, const std::string& path,
    bool preserveRootTransform)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("prefab")));
    JsonValue objectNode = Detail::SceneObjectBehavior::SerializeObject(object, false);
    if (preserveRootTransform && std::filesystem::is_regular_file(path))
    {
        try
        {
            JsonValue existing;
            try
            {
                existing = JsonParseFile(path);
            }
            catch (...)
            {
                pugi::xml_document document;
                if (document.load_file(path.c_str()))
                    existing = Detail::SceneXmlBehavior::ReadNode(document.document_element());
            }
            if (existing["object"]["transform"].IsObject())
                objectNode.Set("transform", existing["object"]["transform"]);
        }
        catch (...)
        {
            // A malformed previous asset should not prevent a valid edit from
            // replacing it; fall back to the source instance transform.
        }
    }
    root.Set("object", std::move(objectNode));
    std::ofstream file(path);
    if (!file)
        return false;
    file << Detail::SceneXmlBehavior::WriteDocument(root, "Prefab");
    return file.good();
}

std::string SceneSerializer::SavePrefabToString(const Engine::Core::Object& object,
    bool includeRootTransform)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("prefab")));
    // A prefab file owns the complete expanded object definition. If this
    // object is already an instance, do not write a self-referencing link.
    JsonValue objectNode = Detail::SceneObjectBehavior::SerializeObject(object, false);
    if (!includeRootTransform)
        objectNode.Set("transform", JsonValue::MakeObject());
    root.Set("object", std::move(objectNode));
    return Detail::SceneXmlBehavior::WriteDocument(root, "Prefab");
}

namespace
{
std::filesystem::path ResolvePrefabPath(const std::string& path)
{
    const std::filesystem::path requested(path);
    if (std::filesystem::exists(requested))
        return requested;

#ifdef ENGINE_ASSETS_PATH
    const std::filesystem::path relativeAsset =
        requested.lexically_relative(std::filesystem::path("Assets"));
    if (!relativeAsset.empty() && *relativeAsset.begin() != "..")
    {
        const std::filesystem::path engineAsset =
            std::filesystem::path(ENGINE_ASSETS_PATH) / relativeAsset;
        if (std::filesystem::exists(engineAsset))
            return engineAsset;
    }
#endif
    return requested;
}

JsonValue LoadPrefabDocument(const std::filesystem::path& path)
{
    try
    {
        return JsonParseFile(path.string());
    }
    catch (...)
    {
        pugi::xml_document document;
        if (!document.load_file(path.string().c_str()))
            throw std::runtime_error("Could not parse prefab file: " + path.string());
        const pugi::xml_node root = document.document_element();
        if (!root)
            throw std::runtime_error("Prefab XML has no root element: " + path.string());
        return Detail::SceneXmlBehavior::ReadNode(root);
    }
}
}

Engine::Core::Object* SceneSerializer::InstantiatePrefab(
    Engine::Scene::Scene& scene,
    const std::string& path,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    Engine::Core::Object* rootObject = nullptr;
    static thread_local std::vector<std::string> prefabStack;
    std::error_code absoluteError;
    const std::filesystem::path resolvedPath = ResolvePrefabPath(path);
    std::filesystem::path identity = std::filesystem::absolute(resolvedPath, absoluteError);
    if (absoluteError)
        identity = resolvedPath;
    std::string prefabKey = identity.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(prefabKey.begin(), prefabKey.end(), prefabKey.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    if (std::find(prefabStack.begin(), prefabStack.end(), prefabKey) != prefabStack.end())
    {
        OutputDebugStringA(("[SceneSerializer] Cyclic prefab reference: " +
            path + "\n").c_str());
        return nullptr;
    }
    prefabStack.push_back(prefabKey);
    struct StackGuard
    {
        std::vector<std::string>& stack;
        ~StackGuard() { stack.pop_back(); }
    } stackGuard{ prefabStack };

    try
    {
        const JsonValue root = LoadPrefabDocument(resolvedPath);
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            root["type"].AsString() != "prefab" || !root["object"].IsObject())
            return nullptr;

        std::vector<std::pair<Engine::Core::Object*, unsigned>> legacyNodeBindings;
        Detail::SceneObjectBehavior::DeserializeOptions options;
        options.assignDefaultNameIfMissing = true;
        options.allowLegacyModelBindings = true;
        options.legacyModelBindings = &legacyNodeBindings;
        rootObject = Detail::SceneObjectBehavior::DeserializeObjectTree(
            scene,
            root["object"],
            graphicsProvider,
            GetRegistry(),
            options);
        if (!rootObject)
            return nullptr;
        if (!legacyNodeBindings.empty())
        {
            Engine::Components::Model* model = rootObject->GetComponent<Engine::Components::Model>();
            if (!model) model = rootObject->AddComponent<Engine::Components::Model>();
            for (const auto& [object, index] : legacyNodeBindings)
                model->BindNode(index, object);
        }
        Detail::SceneObjectBehavior::SetPrefabSourceSnapshot(*rootObject, Detail::SceneObjectBehavior::SerializeObject(*rootObject, false));
        rootObject->SetPrefab(path);
        return rootObject;
    }
    catch (const std::exception& error)
    {
        if (rootObject)
            scene.RemoveObject(rootObject);
        OutputDebugStringA((std::string("[SceneSerializer] Failed to instantiate prefab '") +
            path + "': " + error.what() + "\n").c_str());
        return nullptr;
    }
}

bool SceneSerializer::RefreshPrefabInstances(
    Engine::Scene::Scene& scene,
    const std::string& path,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    Engine::Core::Object* preservedInstance)
{
    EnsureBuiltinsRegistered();
    try
    {
        const std::filesystem::path resolvedPath = ResolvePrefabPath(path);
        const JsonValue root = LoadPrefabDocument(resolvedPath);
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            root["type"].AsString() != "prefab" || !root["object"].IsObject())
            return false;

        auto identity = [](const std::string& value)
        {
            std::error_code error;
            std::filesystem::path result =
                std::filesystem::absolute(value, error);
            if (error)
                result = std::filesystem::path(value);
            std::string key = result.lexically_normal().generic_string();
#ifdef _WIN32
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
#endif
            return key;
        };

        const std::string targetKey = identity(resolvedPath.string());
        std::vector<Engine::Core::Object*> instances;
        for (const auto& candidate : scene.GetObjects())
            if (candidate.get() != preservedInstance && candidate->Prefab &&
                identity(ResolvePrefabPath(
                    candidate->Prefab->GetPath()).string()) == targetKey)
                instances.push_back(candidate.get());

        for (Engine::Core::Object* instance : instances)
        {
            const JsonValue localOverrides = Detail::SceneObjectBehavior::BuildPrefabPatches(*instance, false);
            const Engine::Components::Transform placement = instance->transform;
            const auto prefab = instance->Prefab;
            const std::vector<Engine::Core::Object*> oldChildren = instance->Children;
            for (Engine::Core::Object* child : oldChildren)
                scene.RemoveObject(child);
            instance->Children.clear();
            for (Engine::Core::Component* component : instance->Components)
                delete component;
            instance->Components.clear();

            std::vector<std::pair<Engine::Core::Object*, unsigned>> bindings;
            Detail::SceneObjectBehavior::DeserializeOptions options;
            options.assignDefaultNameIfMissing = true;
            options.allowLegacyModelBindings = true;
            options.legacyModelBindings = &bindings;
            if (!Detail::SceneObjectBehavior::DeserializeObjectTreeInto(
                *instance,
                scene,
                root["object"],
                graphicsProvider,
                GetRegistry(),
                options))
                return false;
            if (!bindings.empty())
            {
                Engine::Components::Model* model = instance->GetComponent<Engine::Components::Model>();
                if (!model) model = instance->AddComponent<Engine::Components::Model>();
                for (const auto& [object, index] : bindings)
                    model->BindNode(index, object);
            }
            // Keep the baseline in the current schema. Legacy ModelNode and
            // MorphTargets entries have already been migrated while populating.
            Detail::SceneObjectBehavior::SetPrefabSourceSnapshot(*instance, Detail::SceneObjectBehavior::SerializeObject(*instance, false));
            Detail::SceneObjectBehavior::ApplyPrefabPatches(*instance, localOverrides, graphicsProvider);
            instance->transform = placement;
            instance->Prefab = prefab;
            instance->PrefabOverrideCacheValid = false;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA((
            std::string("[SceneSerializer] Failed to refresh prefab '") +
            path + "': " + error.what() + "\n").c_str());
        return false;
    }
}

bool SceneSerializer::HasPrefabOverrides(const Engine::Core::Object& instance,
    bool includeRootTransform)
{
    const Engine::Core::Object* root = instance.GetPrefabInstanceRoot();
    if (!root || !root->Prefab) return false;
    Detail::SceneObjectBehavior::EnsurePrefabPatchCache(*root);
    if (!includeRootTransform)
        return root->PrefabHasNonTransformOverrides;
    root->PrefabHasOverrides = root->PrefabHasNonTransformOverrides ||
        Detail::SceneObjectBehavior::PrefabRootTransformDiffers(*root);
    return root->PrefabHasOverrides;
}

bool SceneSerializer::RevertPrefabOverrides(Engine::Core::Object& instance,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Engine::Core::Object* root = instance.GetPrefabInstanceRoot();
    if (!root || !root->Prefab || root->PrefabSourceSnapshot.empty()) return false;
    try
    {
        const bool result = Detail::SceneObjectBehavior::ApplyObjectState(*root,
            JsonParse(root->PrefabSourceSnapshot), graphicsProvider);
        root->PrefabOverrideCacheValid = false;
        return result;
    }
    catch (...) { return false; }
}

bool SceneSerializer::ApplyPrefabOverridesToAsset(Engine::Core::Object& instance,
    bool includeRootTransform, Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Engine::Core::Object* root = instance.GetPrefabInstanceRoot();
    if (!root || !root->Prefab) return false;
    const std::string path = root->Prefab->GetPath();
    if (!SavePrefab(*root, path, !includeRootTransform)) return false;
    try
    {
        Detail::SceneObjectBehavior::SetPrefabSourceSnapshot(*root,
            LoadPrefabDocument(ResolvePrefabPath(path))["object"]);
    }
    catch (...) { return false; }
    root->PrefabOverrideCacheValid = false;
    Engine::Scene::Scene* scene = root->GetScene();
    return !scene || RefreshPrefabInstances(*scene, path, graphicsProvider, root);
}
}
