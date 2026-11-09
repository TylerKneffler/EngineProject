#include "Core/Serialization/SceneSerializerInternal.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include "Core/Serialization/PreparedPrefab.h"
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
#include "Core/Model/RigAsset.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/RenderJobs.h"
#include <pugixml.hpp>
#include <array>
#include <fstream>
#include <future>
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
namespace
{
using RigSave = std::pair<std::string, Engine::Model::RigAsset>;

bool CaptureRigAssets(const Engine::Core::Object& object,
    std::vector<RigSave>& rigs)
{
    for (const auto* component : object.Components)
        if (const auto* skeleton = dynamic_cast<const
                Engine::Components::Skeleton*>(component);
            skeleton && !skeleton->rigPath.empty() &&
            std::filesystem::path(skeleton->rigPath).extension() != ".skeleton")
        {
            const auto* scene = object.GetScene();
            if (!scene) return false;
            auto rig = Engine::Model::RigAsset::Capture(*scene, *skeleton);
            rigs.emplace_back(skeleton->rigPath, std::move(rig));
        }
    for (const auto* child : object.Children)
        if (child && !child->Prefab && !CaptureRigAssets(*child, rigs))
            return false;
    return true;
}

bool SaveRigAssets(std::vector<RigSave>& rigs, const std::string& prefabPath)
{
    for (auto& [path, rig] : rigs)
    {
        if (auto existing = Engine::Model::RigAsset::Load(path, rig.skinIndex))
        {
            rig.animationPaths = std::move(existing->animationPaths);
            if (!existing->bindNodes.empty())
                rig.bindNodes = std::move(existing->bindNodes);
            rig.controls = existing->controls;
            rig.poses = existing->poses;
            rig.symmetryPairs = existing->symmetryPairs;
        }
        std::filesystem::path referencedPrefab(prefabPath);
        const std::filesystem::path storedRig(path);
        if (storedRig.is_relative() && !storedRig.parent_path().empty())
        {
            const auto rigDirectory = std::filesystem::path(
                Engine::Components::Mesh::ResolveFilePath(
                    storedRig.parent_path().string()));
            const auto prefabDirectory = std::filesystem::path(
                Engine::Components::Mesh::ResolveFilePath(
                    referencedPrefab.parent_path().string()));
            std::error_code directoryError;
            if (std::filesystem::equivalent(rigDirectory,
                    prefabDirectory, directoryError) && !directoryError)
                referencedPrefab = storedRig.parent_path() /
                    referencedPrefab.filename();
        }
        rig.prefabPath = referencedPrefab.generic_string();
        if (!rig.Save(path)) return false;
    }
    return true;
}

void RemoveRigBackedInlineData(JsonValue& object)
{
    auto& components = object["components"];
    for (size_t i = 0; i < components.ArraySize(); ++i)
    {
        auto& component = components.ArrayAt(i);
        if (component["type"].AsString() == "Skeleton" &&
            !component["rigPath"].AsString().empty() &&
            std::filesystem::path(component["rigPath"].AsString()).extension() != ".skeleton")
        {
            component.Erase("joints");
            component.Erase("inverseBindMatrices");
        }
    }
    auto& children = object["children"];
    for (size_t i = 0; i < children.ArraySize(); ++i)
        if (children.ArrayAt(i)["components"].IsArray())
            RemoveRigBackedInlineData(children.ArrayAt(i));
}
}

bool SceneSerializer::SavePrefab(const Engine::Core::Object& object, const std::string& path,
    bool preserveRootTransform)
{
    auto save = PreparePrefabSave(object, path, preserveRootTransform);
    return std::async(std::launch::async, std::move(save)).get();
}

std::function<bool()> SceneSerializer::PreparePrefabSave(
    const Engine::Core::Object& object, const std::string& path,
    bool preserveRootTransform)
{
    std::vector<RigSave> rigs;
    if (!CaptureRigAssets(object, rigs)) return [] { return false; };
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("prefab")));
    JsonValue objectNode = Detail::SceneObjectBehavior::SerializeObject(object, false);
    RemoveRigBackedInlineData(objectNode);
    return [root = std::move(root), objectNode = std::move(objectNode),
            rigs = std::move(rigs), path, preserveRootTransform]() mutable {
    if (!SaveRigAssets(rigs, path)) return false;
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
    };
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
    return std::async(std::launch::async, [root = std::move(root)] {
        return Detail::SceneXmlBehavior::WriteDocument(root, "Prefab");
    }).get();
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

std::string PrefabDocumentKey(const std::filesystem::path& path)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    std::string key = (error ? path : absolute).lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return key;
}

JsonValue LoadPrefabDocument(const std::filesystem::path& path)
{
    // Worker reads have their own profile; synchronous reads join the caller.
    PrefabLoadProfile profile(path.generic_string() + " [document conversion]");
    std::string source;
    {
        PrefabLoadProfile::Stage stage("document_file_read");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Could not read prefab file: " + path.string());
        const auto length = file.tellg();
        if (length < 0) throw std::runtime_error("Could not size prefab file: " + path.string());
        source.resize(static_cast<size_t>(length), '\0');
        file.seekg(0);
        if (!file.read(source.data(), static_cast<std::streamsize>(source.size())))
            throw std::runtime_error("Could not read prefab file: " + path.string());
    }
    const auto first = source.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || source[first] != '<')
    {
        try { PrefabLoadProfile::Stage stage("document_json_parse"); return JsonParse(source); }
        catch (...) {} // Preserve XML BOM/encoding support through pugixml.
    }
    {
        pugi::xml_document document;
        // source already owns a mutable file buffer and outlives document.
        // ReadNode creates owned JSON values before either buffer is released.
        // Let pugixml parse it directly instead of copying the whole file again.
        {
            PrefabLoadProfile::Stage stage("document_xml_parse");
            if (!document.load_buffer_inplace(source.data(), source.size()))
                throw std::runtime_error("Could not parse prefab file: " + path.string());
        }
        const pugi::xml_node root = document.document_element();
        if (!root)
            throw std::runtime_error("Prefab XML has no root element: " + path.string());
        PrefabLoadProfile::Stage stage("document_xml_to_json");
        return Detail::SceneXmlBehavior::ReadNode(root);
    }
}

struct CachedPrefabDocument
{
    std::string key;
    std::filesystem::file_time_type modified{};
    std::uintmax_t size = 0;
    JsonValue document;
    std::string sourceSnapshot;
    Engine::Graphics::IGraphicsProvider* sourceProvider = nullptr;
    glm::vec3 sourcePosition { 0.f };
    glm::vec3 sourceRotation { 0.f };
    glm::vec3 sourceScale { 1.f };
};

thread_local std::array<std::shared_ptr<CachedPrefabDocument>, 8> prefabCache;
thread_local std::size_t nextPrefabCache = 0;

std::shared_ptr<CachedPrefabDocument> FindCachedPrefabDocument(
    const std::filesystem::path& path, const std::string& key)
{
    std::error_code modifiedError, sizeError;
    const auto modified = std::filesystem::last_write_time(path, modifiedError);
    const auto size = std::filesystem::file_size(path, sizeError);
    if (modifiedError || sizeError) return {};
    for (const auto& entry : prefabCache)
        if (entry && entry->key == key &&
            entry->modified == modified && entry->size == size)
            return entry;
    return {};
}

std::shared_ptr<CachedPrefabDocument> LoadPrefabDocumentCached(
    const std::filesystem::path& path, const std::string& key,
    const JsonValue* parsed = nullptr)
{
    // Large scenes can place the same prefab hundreds of times. Keep a small
    // owner-thread cache of parsed documents, keyed by the file's current
    // version. Each instantiation still gets independent component objects.
    std::error_code modifiedError, sizeError;
    const auto modified = std::filesystem::last_write_time(path, modifiedError);
    const auto size = std::filesystem::file_size(path, sizeError);
    auto create = [&] {
        auto entry = std::make_shared<CachedPrefabDocument>();
        entry->key = key;
        entry->modified = modified;
        entry->size = size;
        entry->document = parsed ? PreparedPrefab::CopyOrConsumeDocument(*parsed) : LoadPrefabDocument(path);
        return entry;
    };
    if (modifiedError || sizeError)
        return create();
    for (const auto& entry : prefabCache)
        if (entry && entry->key == key &&
            entry->modified == modified && entry->size == size)
            return entry;
    auto document = create();
    prefabCache[nextPrefabCache] = document;
    nextPrefabCache = (nextPrefabCache + 1) % prefabCache.size();
    return document;
}
}

Engine::Core::Object* SceneSerializer::InstantiatePrefab(
    Engine::Scene::Scene& scene,
    const std::string& path,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    PrefabLoadProfile profile(path);
    try
    {
        if (const auto* prepared = PreparedPrefab::FindDocument(path))
            return InstantiatePrefabFromValue(scene, path, *prepared, graphicsProvider);
        const auto resolved = ResolvePrefabPath(path);
        const std::string key = PrefabDocumentKey(resolved);
        if (const auto cached = FindCachedPrefabDocument(resolved, key))
            return InstantiatePrefabFromValue(scene, path, cached->document,
                graphicsProvider);
        PreparedPrefab transient;
        transient.path = path;
        transient.document = [&] {
            PrefabLoadProfile::Stage stage("read_parse");
            return Engine::Rendering::RenderJobs::Get().Submit([resolved] {
                return LoadPrefabDocument(resolved);
            }).get();
        }();
        // Transfer the worker result into the owner's cache through the same
        // single-consumer publication path used by asynchronous placement.
        // No mutable cache entry or live component crosses the worker boundary.
        PreparedPrefab::Publication publication(transient);
        return InstantiatePrefabFromValue(scene, path, transient.document, graphicsProvider);
    }
    catch (const std::exception&) { return nullptr; }
}

JsonValue SceneSerializer::ReadPrefabFile(const std::string& path)
{
    const auto resolved = ResolvePrefabPath(path);
    const std::string key = PrefabDocumentKey(resolved);
    return LoadPrefabDocumentCached(resolved, key)->document;
}

Engine::Core::Object* SceneSerializer::InstantiatePrefabFromValue(
    Engine::Scene::Scene& scene,
    const std::string& path, const JsonValue& parsed,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    PrefabLoadProfile profile(path);
    EnsureBuiltinsRegistered();
    Engine::Core::Object* rootObject = nullptr;
    static thread_local std::vector<std::string> prefabStack;
    const std::filesystem::path resolvedPath = ResolvePrefabPath(path);
    const std::string prefabKey = PrefabDocumentKey(resolvedPath);
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
        const auto document = LoadPrefabDocumentCached(resolvedPath, prefabKey,
            parsed.IsObject() ? &parsed : nullptr);
        const JsonValue& root = document->document;
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            root["type"].AsString() != "prefab" || !root["object"].IsObject())
            return nullptr;

        std::vector<std::pair<Engine::Core::Object*, unsigned>> legacyNodeBindings;
        Detail::SceneObjectBehavior::DeserializeOptions options;
        options.assignDefaultNameIfMissing = true;
        options.allowLegacyModelBindings = true;
        options.legacyModelBindings = &legacyNodeBindings;
        {
            PrefabLoadProfile::Stage stage("deserialize_tree");
            rootObject = Detail::SceneObjectBehavior::DeserializeObjectTree(
                scene,
                root["object"],
                graphicsProvider,
                GetRegistry(),
                options);
        }
        if (!rootObject)
            return nullptr;
        if (!legacyNodeBindings.empty())
        {
            Engine::Components::Model* model = rootObject->GetComponent<Engine::Components::Model>();
            if (!model) model = rootObject->AddComponent<Engine::Components::Model>();
            for (const auto& [object, index] : legacyNodeBindings)
                model->BindNode(index, object);
        }
        if (document->sourceSnapshot.empty() ||
            document->sourceProvider != graphicsProvider)
        {
            PrefabLoadProfile::Stage stage("source_snapshot");
            JsonValue source;
            {
                PrefabLoadProfile::Stage capture("source_snapshot_capture");
                source = Detail::SceneObjectBehavior::SerializeObject(*rootObject, false);
            }
            {
                PrefabLoadProfile::Stage encode("source_snapshot_encode");
                Detail::SceneObjectBehavior::SetPrefabSourceSnapshot(*rootObject, source);
            }
            document->sourceSnapshot = rootObject->PrefabSourceSnapshot;
            document->sourceProvider = graphicsProvider;
            document->sourcePosition = rootObject->PrefabSourcePosition;
            document->sourceRotation = rootObject->PrefabSourceRotation;
            document->sourceScale = rootObject->PrefabSourceScale;
        }
        else
        {
            rootObject->PrefabSourceSnapshot = document->sourceSnapshot;
            rootObject->PrefabSourcePosition = document->sourcePosition;
            rootObject->PrefabSourceRotation = document->sourceRotation;
            rootObject->PrefabSourceScale = document->sourceScale;
            rootObject->PrefabSourceTransformValid = true;
        }
        // The baseline was captured from this exact instance. Its first
        // inspector/undo query does not need to parse and diff it again.
        rootObject->PrefabOverridePatchSnapshot.clear();
        rootObject->PrefabHasNonTransformOverrides = false;
        rootObject->PrefabHasOverrides = false;
        rootObject->PrefabOverrideCacheValid = true;
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
        const JsonValue root = Engine::Rendering::RenderJobs::Get().Submit(
            [resolvedPath] { return LoadPrefabDocument(resolvedPath); }).get();
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
            instance->AssertSceneOwner();
            const JsonValue localOverrides = Detail::SceneObjectBehavior::BuildPrefabPatches(*instance, false);
            const Engine::Components::Transform placement = instance->transform;
            const auto prefab = instance->Prefab;
            const std::vector<Engine::Core::Object*> oldChildren = instance->Children;
            for (Engine::Core::Object* child : oldChildren)
                scene.RemoveObject(child);
            instance->Children.clear();
            scene.WaitForGpuResourceDestruction();
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
