#pragma once

#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"

#include <string>
#include <unordered_map>

namespace pugi { class xml_node; }
namespace Engine::Components { class Transform; }

namespace Engine::Serialization::Detail
{
// XML is a transport concern: it converts the serializer's format-neutral
// JsonValue tree to and from the on-disk XML representation.
class SceneXmlBehavior
{
public:
    static JsonValue ReadNode(const pugi::xml_node& node);
    static std::string WriteDocument(const JsonValue& value,
        const char* rootName = "Scene");
    static bool LoadDocument(Engine::Scene::Scene& scene,
        const std::string& source,
        Engine::Graphics::IGraphicsProvider* graphicsProvider,
        const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
        bool isFile);

private:
    static void WriteNode(pugi::xml_node node, const JsonValue& value);
    static void WriteField(pugi::xml_node parent, const char* name,
        const JsonValue& value);
    static void WriteObjectFields(pugi::xml_node parent,
        const JsonValue& value, const char* skipKey = nullptr,
        const char* ownerName = nullptr);
    static void WriteArrayItems(pugi::xml_node parent,
        const JsonValue& value, const char* itemName);
};

// Component lookup and legacy case matching are isolated from graph loading.
class ComponentRegistryBehavior
{
public:
    static const SceneSerializer::Factory* FindFactory(
        const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
        const std::string& typeName);
};

// Encodes and applies individual object graphs. It also owns the structural
// prefab-diff representation because those patches operate on object trees.
class SceneObjectBehavior
{
public:
    static JsonValue SerializeTransform(
        const Engine::Components::Transform& transform);
    static void DeserializeTransform(Engine::Components::Transform& transform,
        const JsonValue& node);
    static JsonValue SerializeObject(const Engine::Core::Object& object,
        bool serializePrefabAsReference = true);
    static JsonValue SerializeScene(const Engine::Scene::Scene& scene);
    static void SetPrefabSourceSnapshot(Engine::Core::Object& object,
        const JsonValue& source);
    static bool PrefabRootTransformDiffers(
        const Engine::Core::Object& instance);
    static JsonValue SerializeBakedMaterialOverrides(
        const Engine::Core::Object& root);
    static void ApplyBakedMaterialOverrides(Engine::Core::Object& root,
        const JsonValue& node,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
    static JsonValue BuildPrefabPatches(const Engine::Core::Object& instance,
        bool includeRootTransform);
    static void EnsurePrefabPatchCache(const Engine::Core::Object& instance);
    static bool ApplyPrefabPatches(Engine::Core::Object& instance,
        const JsonValue& patches,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
    static bool ApplyObjectState(Engine::Core::Object& object,
        const JsonValue& node,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);

private:
    static bool JsonEquals(const JsonValue& left, const JsonValue& right);
    static JsonValue PathWith(const JsonValue& path, const std::string& key);
    static JsonValue PathWith(const JsonValue& path, size_t index);
    static void AddPatch(JsonValue& patches, const JsonValue& path,
        const JsonValue* value);
    static void DiffJson(const JsonValue& baseline, const JsonValue& current,
        const JsonValue& path, JsonValue& patches, bool skipRootTransform);
    static bool ApplyPatch(JsonValue& target, const JsonValue& patch);
};

// Scene-level loading coordinates settings, root objects, nested prefabs and
// component factories while delegating value/object details to the behaviors.
class SceneDocumentBehavior
{
public:
    static bool DeserializeScene(Engine::Scene::Scene& scene,
        const JsonValue& root,
        Engine::Graphics::IGraphicsProvider* graphicsProvider,
        const std::unordered_map<std::string, SceneSerializer::Factory>& registry);
};
}
