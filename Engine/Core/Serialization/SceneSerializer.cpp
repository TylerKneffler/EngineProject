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
static bool DeserialiseScene(Engine::Scene::Scene& scene, const JsonValue& root,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry);

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

// ---- Local GLM helpers ------------------------------------------------------
namespace
{

namespace SceneXml
{
constexpr const char* ComponentPrefix = "components";
constexpr const char* ComponentNamespace = "urn:engine-components";
constexpr const char* TypeField = "type";
constexpr const char* ArrayItem = "item";

// Files written before arrays became self-describing used semantic collection
// element names. Keep those names here only as an input compatibility rule;
// newly serialized arrays do not depend on them.
bool IsLegacyCollection(const std::string& name)
{
    return name == "objects" || name == "components" || name == "children";
}
}

bool SplitQualifiedName(const std::string& name, std::string& prefix, std::string& localName)
{
    const std::size_t colon = name.find(':');
    if (colon == std::string::npos)
        return false;
    prefix = name.substr(0, colon);
    localName = name.substr(colon + 1);
    return true;
}

const SceneSerializer::Factory* FindComponentFactory(
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
    const std::string& typeName)
{
    const auto exact = registry.find(typeName);
    if (exact != registry.end())
        return &exact->second;

    // Older XML writers normalized component element casing. Preserve input
    // compatibility without changing the class-defined serialized type name.
    const auto sameIgnoringCase = [&typeName](const auto& entry)
    {
        if (entry.first.size() != typeName.size())
            return false;
        return std::equal(entry.first.begin(), entry.first.end(), typeName.begin(),
            [](unsigned char left, unsigned char right)
            {
                return std::tolower(left) == std::tolower(right);
            });
    };
    const auto legacy = std::find_if(registry.begin(), registry.end(), sameIgnoringCase);
    return legacy == registry.end() ? nullptr : &legacy->second;
}

std::string StripOwnedPrefix(const std::string& childName)
{
    const std::size_t dot = childName.rfind('.');
    if (dot == std::string::npos)
        return childName;
    return childName.substr(dot + 1);
}

JsonValue ParseScalarText(const std::string& text)
{
    if (text == "true")
        return JsonValue(true);
    if (text == "false")
        return JsonValue(false);

    if (!text.empty())
    {
        char* end = nullptr;
        errno = 0;
        const double number = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && end != text.c_str() && errno != ERANGE)
            return JsonValue(number);
    }

    return JsonValue(text);
}

JsonValue ReadXmlNode(const pugi::xml_node& node)
{
    const std::string name = node.name();
    const std::string fieldName = StripOwnedPrefix(name);
    std::string namespacePrefix;
    std::string localName;
    const bool isQualified =
        SplitQualifiedName(name, namespacePrefix, localName);

    if (isQualified && namespacePrefix == SceneXml::ComponentPrefix)
    {
        JsonValue component = JsonValue::MakeObject();
        component.Set(SceneXml::TypeField, JsonValue(localName));
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                component.Set(StripOwnedPrefix(child.name()), ReadXmlNode(child));
        return component;
    }

    bool hasElementChildren = false;
    bool allItems = true;
    bool allComponents = true;
    for (const pugi::xml_node& child : node.children())
    {
        if (child.type() != pugi::node_element)
            continue;
        hasElementChildren = true;
        allItems = allItems && std::string(child.name()) == SceneXml::ArrayItem;
        std::string childPrefix;
        std::string childLocalName;
        allComponents = allComponents &&
            SplitQualifiedName(child.name(), childPrefix, childLocalName) &&
            childPrefix == SceneXml::ComponentPrefix;
    }

    if (SceneXml::IsLegacyCollection(fieldName) ||
        (hasElementChildren && (allItems || allComponents)))
    {
        JsonValue array = JsonValue::MakeArray();
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                array.Push(ReadXmlNode(child));
        return array;
    }

    if (!hasElementChildren)
        return ParseScalarText(node.child_value());

    JsonValue object = JsonValue::MakeObject();
    for (const pugi::xml_node& child : node.children())
    {
        if (child.type() != pugi::node_element)
            continue;
        object.Set(StripOwnedPrefix(child.name()), ReadXmlNode(child));
    }
    return object;
}

void WriteXmlNode(pugi::xml_node node, const JsonValue& value);
void WriteXmlField(pugi::xml_node parent, const char* name, const JsonValue& value);
void WriteXmlObjectFields(pugi::xml_node parent, const JsonValue& value,
    const char* skipKey = nullptr, const char* ownerName = nullptr);
void WriteXmlArrayItems(pugi::xml_node parent, const JsonValue& value, const char* itemName)
{
    for (std::size_t i = 0; i < value.ArraySize(); ++i)
    {
        const JsonValue& item = value.ArrayAt(i);
        if (item.IsObject() && item[SceneXml::TypeField].IsString() &&
            !item[SceneXml::TypeField].AsString().empty())
        {
            const std::string elementName = std::string(SceneXml::ComponentPrefix) +
                ":" + item[SceneXml::TypeField].AsString();
            pugi::xml_node child = parent.append_child(elementName.c_str());
            WriteXmlObjectFields(child, item, SceneXml::TypeField,
                item[SceneXml::TypeField].AsString().c_str());
        }
        else
        {
            pugi::xml_node child = parent.append_child(itemName);
            WriteXmlNode(child, item);
        }
    }
}

void WriteXmlObjectFields(pugi::xml_node parent, const JsonValue& value,
    const char* skipKey, const char* ownerName)
{
    for (std::size_t i = 0; i < value.ObjectSize(); ++i)
    {
        const std::string key = value.ObjectKey(i);
        if (skipKey && key == skipKey)
            continue;
        const std::string fieldName = ownerName && *ownerName
            ? std::string(ownerName) + "." + key
            : key;
        WriteXmlField(parent, fieldName.c_str(), value.ObjectValue(i));
    }
}

void WriteXmlField(pugi::xml_node parent, const char* name, const JsonValue& value)
{
    pugi::xml_node child = parent.append_child(name ? name : "");
    if (value.IsArray())
    {
        WriteXmlArrayItems(child, value, SceneXml::ArrayItem);
        return;
    }
    if (value.IsObject() && value[SceneXml::TypeField].IsString() &&
        !value[SceneXml::TypeField].AsString().empty())
    {
        WriteXmlObjectFields(child, value, SceneXml::TypeField,
            value[SceneXml::TypeField].AsString().c_str());
        return;
    }
    WriteXmlNode(child, value);
}

void WriteXmlNode(pugi::xml_node node, const JsonValue& value)
{
    switch (value.GetType())
    {
    case JsonValue::Type::Null:
        node.text().set("");
        break;
    case JsonValue::Type::Bool:
        node.text().set(value.AsBool() ? "true" : "false");
        break;
    case JsonValue::Type::Number:
    {
        std::ostringstream stream;
        stream << value.AsDouble();
        node.text().set(stream.str().c_str());
        break;
    }
    case JsonValue::Type::String:
        node.text().set(value.AsString().c_str());
        break;
    case JsonValue::Type::Array:
        WriteXmlArrayItems(node, value, "item");
        break;
    case JsonValue::Type::Object:
        WriteXmlObjectFields(node, value);
        break;
    }
}

std::string WriteXmlDocument(const JsonValue& value, const char* rootName = "Scene")
{
    pugi::xml_document document;
    pugi::xml_node root = document.append_child(rootName);
    root.append_attribute("xmlns:components") = SceneXml::ComponentNamespace;
    if (value.IsObject())
        WriteXmlObjectFields(root, value);
    else
        WriteXmlNode(root, value);

    std::ostringstream stream;
    document.save(stream, "  ");
    return stream.str();
}

bool LoadXmlDocument(Engine::Scene::Scene& scene, const std::string& source,
    Engine::Graphics::IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry,
    bool isFile)
{
    pugi::xml_document document;
    pugi::xml_parse_result result = isFile
        ? document.load_file(source.c_str())
        : document.load_string(source.c_str());
    if (!result)
        return false;

    const pugi::xml_node root = document.document_element();
    if (!root)
        return false;

    return DeserialiseScene(scene, ReadXmlNode(root), graphicsProvider, registry);
}

} // namespace

// ---- Serialise a single Engine::Core::Object (recursive) ----------------------------------
static JsonValue SerialiseTransform(const Engine::Components::Transform& transform)
{
    return transform.Serialize();
}

static void DeserialiseTransform(Engine::Components::Transform& transform, const JsonValue& node)
{
    if (!node.IsObject())
        return;
    transform.Deserialize(node);
}

static void SetPrefabSourceSnapshot(Engine::Core::Object& object, const JsonValue& source)
{
    object.PrefabSourceSnapshot = JsonWrite(source);
    Engine::Components::Transform sourceTransform;
    DeserialiseTransform(sourceTransform, source["transform"]);
    object.PrefabSourcePosition = sourceTransform.position;
    object.PrefabSourceRotation = sourceTransform.rotation;
    object.PrefabSourceScale = sourceTransform.scale;
    object.PrefabSourceTransformValid = true;
    object.PrefabOverrideCacheValid = false;
}

static bool PrefabRootTransformDiffers(const Engine::Core::Object& instance)
{
    if (!instance.PrefabSourceTransformValid)
        return false;
    constexpr float epsilon = 0.000001f;
    const auto differs = [epsilon](const glm::vec3& left, const glm::vec3& right)
    {
        return std::abs(left.x - right.x) > epsilon ||
            std::abs(left.y - right.y) > epsilon ||
            std::abs(left.z - right.z) > epsilon;
    };
    return differs(instance.transform.position, instance.PrefabSourcePosition) ||
        differs(instance.transform.rotation, instance.PrefabSourceRotation) ||
        differs(instance.transform.scale, instance.PrefabSourceScale);
}

static JsonValue SerialiseBakedMaterialOverrides(const Engine::Core::Object& root)
{
    JsonValue overrides = JsonValue::MakeArray();
    std::function<void(const Engine::Core::Object&, std::vector<size_t>)> visit =
        [&](const Engine::Core::Object& object, std::vector<size_t> path)
    {
        const auto* baked = object.GetComponent<Engine::Rendering::BakedLightingData>();
        const auto* material = object.GetComponent<Engine::Components::Material>();
        if (baked && material)
        {
            JsonValue entry = JsonValue::MakeObject();
            JsonValue serializedPath = JsonValue::MakeArray();
            for (const size_t index : path)
                serializedPath.Push(JsonValue(static_cast<int>(index)));
            entry.Set("path", std::move(serializedPath));
            entry.Set("material", material->Serialize());
            entry.Set("bakedLighting", baked->Serialize());
            overrides.Push(std::move(entry));
        }
        for (size_t index = 0; index < object.Children.size(); ++index)
        {
            auto childPath = path;
            childPath.push_back(index);
            visit(*object.Children[index], std::move(childPath));
        }
    };
    visit(root, {});
    return overrides;
}

static void ApplyBakedMaterialOverrides(Engine::Core::Object& root, const JsonValue& node,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    const JsonValue& overrides = node["bakedMaterialOverrides"];
    for (size_t overrideIndex = 0;
        overrideIndex < overrides.ArraySize(); ++overrideIndex)
    {
        const JsonValue& entry = overrides.ArrayAt(overrideIndex);
        Engine::Core::Object* object = &root;
        const JsonValue& path = entry["path"];
        bool validPath = true;
        for (size_t depth = 0; depth < path.ArraySize(); ++depth)
        {
            const int index = path.ArrayAt(depth).AsInt();
            if (index < 0 || static_cast<size_t>(index) >= object->Children.size())
            {
                validPath = false;
                break;
            }
            object = object->Children[static_cast<size_t>(index)];
        }
        if (!validPath || !entry["material"].IsObject() ||
            !entry["bakedLighting"].IsObject())
            continue;

        Engine::Components::Material* material = object->GetComponent<Engine::Components::Material>();
        if (!material)
            material = object->AddComponent<Engine::Components::Material>();
        material->Deserialize(entry["material"]);
        material->OnAfterDeserialize(graphicsProvider);

        Engine::Rendering::BakedLightingData* baked = object->GetComponent<Engine::Rendering::BakedLightingData>();
        if (!baked)
            baked = object->AddComponent<Engine::Rendering::BakedLightingData>();
        baked->Deserialize(entry["bakedLighting"]);
    }
}

static JsonValue BuildPrefabPatches(const Engine::Core::Object& instance,
    bool includeRootTransform);
static void EnsurePrefabPatchCache(const Engine::Core::Object& instance);

static JsonValue SerialiseObject(
    const Engine::Core::Object& obj,
    bool serialisePrefabAsReference = true)
{
    JsonValue node = JsonValue::MakeObject();
    if (serialisePrefabAsReference && obj.Prefab)
    {
        node.Set("prefab", JsonValue(obj.Prefab->GetPath()));
        node.Set("transform", SerialiseTransform(obj.transform));
        EnsurePrefabPatchCache(obj);
        JsonValue prefabOverrides = JsonValue::MakeArray();
        try
        {
            if (!obj.PrefabOverridePatchSnapshot.empty())
                prefabOverrides = JsonParse(obj.PrefabOverridePatchSnapshot);
        }
        catch (...) {}
        if (prefabOverrides.ArraySize() > 0)
            node.Set("prefabOverrides", prefabOverrides);
        const JsonValue overrides = SerialiseBakedMaterialOverrides(obj);
        if (overrides.ArraySize() > 0)
            node.Set("bakedMaterialOverrides", overrides);
        return node;
    }

    node.Set("name", JsonValue(obj.name));
    node.Set("enabled", JsonValue(obj.enabled));

    // Engine::Components::Transform
    node.Set("transform", SerialiseTransform(obj.transform));

    // Components
    JsonValue comps = JsonValue::MakeArray();
    for (const Engine::Core::Component* comp : obj.Components)
        comps.Push(comp->Serialize());
    node.Set("components", std::move(comps));

    // Children (recursive)
    JsonValue children = JsonValue::MakeArray();
    for (const Engine::Core::Object* child : obj.Children)
        children.Push(SerialiseObject(*child, true));
    node.Set("children", std::move(children));

    return node;
}

static bool JsonEquals(const JsonValue& left, const JsonValue& right)
{
    if (left.GetType() != right.GetType()) return false;
    switch (left.GetType())
    {
    case JsonValue::Type::Null: return true;
    case JsonValue::Type::Bool: return left.AsBool() == right.AsBool();
    case JsonValue::Type::Number:
    {
        const double a = left.AsDouble();
        const double b = right.AsDouble();
        return std::abs(a - b) <= 0.000001 * std::max({ 1.0, std::abs(a), std::abs(b) });
    }
    case JsonValue::Type::String: return left.AsString() == right.AsString();
    case JsonValue::Type::Array:
        if (left.ArraySize() != right.ArraySize()) return false;
        for (size_t i = 0; i < left.ArraySize(); ++i)
            if (!JsonEquals(left.ArrayAt(i), right.ArrayAt(i))) return false;
        return true;
    case JsonValue::Type::Object:
        if (left.ObjectSize() != right.ObjectSize()) return false;
        for (size_t i = 0; i < left.ObjectSize(); ++i)
        {
            const std::string& key = left.ObjectKey(i);
            if (!right.Has(key) || !JsonEquals(left.ObjectValue(i), right[key]))
                return false;
        }
        return true;
    }
    return false;
}

static JsonValue PathWith(const JsonValue& path, const std::string& key)
{
    JsonValue result = path;
    JsonValue segment = JsonValue::MakeObject();
    segment.Set("key", JsonValue(key));
    result.Push(std::move(segment));
    return result;
}

static JsonValue PathWith(const JsonValue& path, size_t index)
{
    JsonValue result = path;
    JsonValue segment = JsonValue::MakeObject();
    segment.Set("index", JsonValue(static_cast<int>(index)));
    result.Push(std::move(segment));
    return result;
}

static void AddPatch(JsonValue& patches, const JsonValue& path,
    const JsonValue* value)
{
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("path", path);
    if (value) patch.Set("value", *value);
    else patch.Set("remove", JsonValue(true));
    patches.Push(std::move(patch));
}

static void DiffJson(const JsonValue& baseline, const JsonValue& current,
    const JsonValue& path, JsonValue& patches, bool skipRootTransform)
{
    if (baseline.GetType() != current.GetType())
    {
        AddPatch(patches, path, &current);
        return;
    }
    if (baseline.IsObject())
    {
        for (size_t i = 0; i < current.ObjectSize(); ++i)
        {
            const std::string& key = current.ObjectKey(i);
            if (skipRootTransform && path.ArraySize() == 0 && key == "transform")
                continue;
            const JsonValue nextPath = PathWith(path, key);
            if (!baseline.Has(key)) AddPatch(patches, nextPath, &current.ObjectValue(i));
            else DiffJson(baseline[key], current.ObjectValue(i), nextPath,
                patches, skipRootTransform);
        }
        for (size_t i = 0; i < baseline.ObjectSize(); ++i)
        {
            const std::string& key = baseline.ObjectKey(i);
            if ((skipRootTransform && path.ArraySize() == 0 && key == "transform") ||
                current.Has(key)) continue;
            const JsonValue nextPath = PathWith(path, key);
            AddPatch(patches, nextPath, nullptr);
        }
        return;
    }
    if (baseline.IsArray())
    {
        if (baseline.ArraySize() != current.ArraySize())
        {
            AddPatch(patches, path, &current);
            return;
        }
        for (size_t i = 0; i < current.ArraySize(); ++i)
            DiffJson(baseline.ArrayAt(i), current.ArrayAt(i), PathWith(path, i),
                patches, skipRootTransform);
        return;
    }
    if (!JsonEquals(baseline, current)) AddPatch(patches, path, &current);
}

static JsonValue BuildPrefabPatches(const Engine::Core::Object& instance,
    bool includeRootTransform)
{
    JsonValue patches = JsonValue::MakeArray();
    if (instance.PrefabSourceSnapshot.empty()) return patches;
    try
    {
        const JsonValue baseline = JsonParse(instance.PrefabSourceSnapshot);
        const JsonValue current = SerialiseObject(instance, false);
        DiffJson(baseline, current, JsonValue::MakeArray(), patches,
            !includeRootTransform);
    }
    catch (...) {}
    return patches;
}

static void EnsurePrefabPatchCache(const Engine::Core::Object& instance)
{
    if (instance.PrefabOverrideCacheValid) return;
    const JsonValue nonTransform = BuildPrefabPatches(instance, false);
    instance.PrefabOverridePatchSnapshot = JsonWrite(nonTransform);
    instance.PrefabHasNonTransformOverrides = nonTransform.ArraySize() > 0;
    instance.PrefabHasOverrides = instance.PrefabHasNonTransformOverrides ||
        PrefabRootTransformDiffers(instance);
    instance.PrefabOverrideCacheValid = true;
}

static bool ApplyPatch(JsonValue& target, const JsonValue& patch)
{
    const JsonValue& path = patch["path"];
    if (!path.IsArray() || path.ArraySize() == 0) return false;
    JsonValue* node = &target;
    for (size_t i = 0; i + 1 < path.ArraySize(); ++i)
    {
        const JsonValue& segment = path.ArrayAt(i);
        if (segment.Has("key")) node = &(*node)[segment["key"].AsString()];
        else if (segment.Has("index") && node->IsArray() &&
            segment["index"].AsInt() >= 0 &&
            static_cast<size_t>(segment["index"].AsInt()) < node->ArraySize())
            node = &node->ArrayAt(static_cast<size_t>(segment["index"].AsInt()));
        else return false;
    }
    const JsonValue& last = path.ArrayAt(path.ArraySize() - 1);
    if (last.Has("key"))
    {
        const std::string key = last["key"].AsString();
        if (patch.Has("remove")) node->Erase(key);
        else (*node)[key] = patch["value"];
        return true;
    }
    if (last.Has("index") && node->IsArray())
    {
        const int index = last["index"].AsInt();
        if (index < 0 || static_cast<size_t>(index) >= node->ArraySize()) return false;
        node->ArrayAt(static_cast<size_t>(index)) = patch["value"];
        return true;
    }
    return false;
}

static bool ApplyObjectState(Engine::Core::Object& object, const JsonValue& node,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!node.IsObject()) return false;
    if (node.Has("name")) object.name = node["name"].AsString();
    object.enabled = !node.Has("enabled") || node["enabled"].AsBool();
    DeserialiseTransform(object.transform, node["transform"]);
    if (node.Has("prefab")) return true;
    const JsonValue& components = node["components"];
    if (components.ArraySize() != object.Components.size()) return false;
    auto component = object.Components.begin();
    for (size_t i = 0; i < components.ArraySize(); ++i, ++component)
    {
        if (!*component || (*component)->GetTypeName() !=
            components.ArrayAt(i)["type"].AsString()) return false;
        (*component)->Deserialize(components.ArrayAt(i));
        (*component)->OnAfterDeserialize(graphicsProvider);
    }
    const JsonValue& children = node["children"];
    if (children.ArraySize() != object.Children.size()) return false;
    for (size_t i = 0; i < children.ArraySize(); ++i)
        if (!ApplyObjectState(*object.Children[i], children.ArrayAt(i), graphicsProvider))
            return false;
    return true;
}

static bool ApplyPrefabPatches(Engine::Core::Object& instance, const JsonValue& patches,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (instance.PrefabSourceSnapshot.empty()) return false;
    JsonValue merged;
    try { merged = JsonParse(instance.PrefabSourceSnapshot); }
    catch (...) { return false; }
    for (size_t i = 0; i < patches.ArraySize(); ++i)
        if (!ApplyPatch(merged, patches.ArrayAt(i))) return false;
    const Engine::Components::Transform placement = instance.transform;
    const bool applied = ApplyObjectState(instance, merged, graphicsProvider);
    instance.transform = placement;
    return applied;
}

static JsonValue SerialiseScene(const Engine::Scene::Scene& scene)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));

    // Settings
    root.Set("settings", Engine::Scene::SerializeSceneSettings(scene.settings));

    // Objects — only root-level (no parent)
    JsonValue objects = JsonValue::MakeArray();
    for (const auto& obj : scene.GetObjects())
        if (obj->Parent == nullptr)
            objects.Push(SerialiseObject(*obj));
    root.Set("objects", std::move(objects));

    return root;
}

// ---- Save -------------------------------------------------------------------
std::string SceneSerializer::SaveToString(const Engine::Scene::Scene& scene)
{
    return WriteXmlDocument(SerialiseScene(scene));
}

bool SceneSerializer::Save(const Engine::Scene::Scene& scene, const std::string& path)
{
    std::ofstream file(path);
    if (!file) return false;
    file << SaveToString(scene);
    return file.good();
}

static bool DeserialiseScene(Engine::Scene::Scene& scene, const JsonValue& root,
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
        DeserialiseTransform(obj.transform, node["transform"]);

        // Components
        const JsonValue& comps = node["components"];
        for (std::size_t i = 0; i < comps.ArraySize(); ++i)
        {
            const JsonValue& cn  = comps.ArrayAt(i);
            const std::string& t = cn["type"].AsString();

            const SceneSerializer::Factory* factory = FindComponentFactory(registry, t);
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
                DeserialiseTransform(child->transform, childNode["transform"]);
                if (childNode.Has("enabled"))
                    child->enabled = childNode["enabled"].AsBool();
                if (childNode.Has("prefabOverrides"))
                    ApplyPrefabPatches(*child, childNode["prefabOverrides"],
                        graphicsProvider);
                ApplyBakedMaterialOverrides(
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
            DeserialiseTransform(object->transform, objectNode["transform"]);
            if (objectNode.Has("enabled"))
                object->enabled = objectNode["enabled"].AsBool();
            if (objectNode.Has("prefabOverrides"))
                ApplyPrefabPatches(*object, objectNode["prefabOverrides"],
                    graphicsProvider);
            ApplyBakedMaterialOverrides(
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
        if (DeserialiseScene(scene, JsonParse(source), graphicsProvider, GetRegistry()))
            return true;
    }
    catch (const std::exception&) {}

    try
    {
        if (LoadXmlDocument(scene, source, graphicsProvider, GetRegistry(), false))
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
    try { return LoadXmlDocument(scene, path, graphicsProvider, GetRegistry(), true); }
    catch (const std::exception&) {}

    try { return DeserialiseScene(scene, JsonParseFile(path), graphicsProvider, GetRegistry()); }
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
    root.Set("object", SerialiseObject(object, true));
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
                DeserialiseTransform(object->transform, node["transform"]);
                if (node.Has("enabled"))
                    object->enabled = node["enabled"].AsBool();
                if (node.Has("prefabOverrides"))
                    ApplyPrefabPatches(*object, node["prefabOverrides"],
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
            DeserialiseTransform(object->transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t index = 0; index < components.ArraySize(); ++index)
            {
                const JsonValue& componentNode = components.ArrayAt(index);
                const SceneSerializer::Factory* factory = FindComponentFactory(
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

bool SceneSerializer::SavePrefab(const Engine::Core::Object& object, const std::string& path,
    bool preserveRootTransform)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("prefab")));
    JsonValue objectNode = SerialiseObject(object, false);
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
                    existing = ReadXmlNode(document.document_element());
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
    file << WriteXmlDocument(root, "Prefab");
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
    JsonValue objectNode = SerialiseObject(object, false);
    if (!includeRootTransform)
        objectNode.Set("transform", JsonValue::MakeObject());
    root.Set("object", std::move(objectNode));
    return WriteXmlDocument(root, "Prefab");
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
        return ReadXmlNode(root);
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
        std::function<void(Engine::Core::Object&, const JsonValue&)> instantiate =
            [&](Engine::Core::Object& object, const JsonValue& node)
        {
            if (node.Has("name"))
                object.name = node["name"].AsString();
            object.enabled = !node.Has("enabled") || node["enabled"].AsBool();

            DeserialiseTransform(object.transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t i = 0; i < components.ArraySize(); ++i)
            {
                const JsonValue& componentNode = components.ArrayAt(i);
                const std::string type = componentNode["type"].AsString();
                const SceneSerializer::Factory* factory = FindComponentFactory(GetRegistry(), type);
                if (!factory)
                {
                    if (type == "ModelNode")
                        legacyNodeBindings.emplace_back(&object,
                            static_cast<unsigned>(componentNode["nodeIndex"].AsInt()));
                    else if (type == "MorphTargets")
                        if (Engine::Components::Mesh* mesh = object.GetComponent<Engine::Components::Mesh>())
                            mesh->DeserializeLegacyMorphTargets(componentNode);
                    continue;
                }

                Engine::Core::Component* component = (*factory)();
                component->Owner = &object;
                component->Deserialize(componentNode);
                component->OnAfterDeserialize(graphicsProvider);
                object.Components.push_back(component);
            }

            const JsonValue& children = node["children"];
            for (std::size_t i = 0; i < children.ArraySize(); ++i)
            {
                const JsonValue& childNode = children.ArrayAt(i);
                Engine::Core::Object* child = nullptr;
                if (childNode.Has("prefab"))
                {
                    child = InstantiatePrefab(
                        scene, childNode["prefab"].AsString(), graphicsProvider);
                    if (!child)
                        throw std::runtime_error(
                            "Could not instantiate nested prefab '" +
                            childNode["prefab"].AsString() + "'");
                    DeserialiseTransform(child->transform, childNode["transform"]);
                    if (childNode.Has("enabled"))
                        child->enabled = childNode["enabled"].AsBool();
                    ApplyBakedMaterialOverrides(
                        *child, childNode, graphicsProvider);
                }
                else
                {
                    child = scene.AddObject();
                    instantiate(*child, childNode);
                }
                child->Parent = &object;
                object.Children.push_back(child);
            }
        };

        rootObject = scene.AddObject();
        instantiate(*rootObject, root["object"]);
        if (!legacyNodeBindings.empty())
        {
            Engine::Components::Model* model = rootObject->GetComponent<Engine::Components::Model>();
            if (!model) model = rootObject->AddComponent<Engine::Components::Model>();
            for (const auto& [object, index] : legacyNodeBindings)
                model->BindNode(index, object);
        }
        SetPrefabSourceSnapshot(*rootObject, SerialiseObject(*rootObject, false));
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

        std::vector<std::pair<Engine::Core::Object*, unsigned>>* legacyNodeBindings = nullptr;
        std::function<void(Engine::Core::Object&, const JsonValue&)> populate =
            [&](Engine::Core::Object& object, const JsonValue& node)
        {
            object.name = node.Has("name")
                ? node["name"].AsString()
                : std::string("Object");
            object.enabled = !node.Has("enabled") || node["enabled"].AsBool();
            DeserialiseTransform(object.transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t index = 0; index < components.ArraySize(); ++index)
            {
                const JsonValue& componentNode = components.ArrayAt(index);
                const std::string type = componentNode["type"].AsString();
                const SceneSerializer::Factory* factory = FindComponentFactory(GetRegistry(), type);
                if (!factory)
                {
                    if (legacyNodeBindings && type == "ModelNode")
                        legacyNodeBindings->emplace_back(&object,
                            static_cast<unsigned>(componentNode["nodeIndex"].AsInt()));
                    else if (type == "MorphTargets")
                        if (Engine::Components::Mesh* mesh = object.GetComponent<Engine::Components::Mesh>())
                            mesh->DeserializeLegacyMorphTargets(componentNode);
                    continue;
                }
                Engine::Core::Component* component = (*factory)();
                component->Owner = &object;
                component->Deserialize(componentNode);
                component->OnAfterDeserialize(graphicsProvider);
                object.Components.push_back(component);
            }

            const JsonValue& children = node["children"];
            for (std::size_t index = 0; index < children.ArraySize(); ++index)
            {
                const JsonValue& childNode = children.ArrayAt(index);
                Engine::Core::Object* child = nullptr;
                if (childNode.Has("prefab"))
                {
                    child = InstantiatePrefab(
                        scene, childNode["prefab"].AsString(), graphicsProvider);
                    if (!child)
                        throw std::runtime_error(
                            "Could not refresh nested prefab '" +
                            childNode["prefab"].AsString() + "'");
                    DeserialiseTransform(child->transform, childNode["transform"]);
                    if (childNode.Has("enabled"))
                        child->enabled = childNode["enabled"].AsBool();
                    ApplyBakedMaterialOverrides(
                        *child, childNode, graphicsProvider);
                }
                else
                {
                    child = scene.AddObject();
                    populate(*child, childNode);
                }
                child->Parent = &object;
                object.Children.push_back(child);
            }
        };

        for (Engine::Core::Object* instance : instances)
        {
            const JsonValue localOverrides = BuildPrefabPatches(*instance, false);
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
            legacyNodeBindings = &bindings;
            populate(*instance, root["object"]);
            legacyNodeBindings = nullptr;
            if (!bindings.empty())
            {
                Engine::Components::Model* model = instance->GetComponent<Engine::Components::Model>();
                if (!model) model = instance->AddComponent<Engine::Components::Model>();
                for (const auto& [object, index] : bindings)
                    model->BindNode(index, object);
            }
            // Keep the baseline in the current schema. Legacy ModelNode and
            // MorphTargets entries have already been migrated while populating.
            SetPrefabSourceSnapshot(*instance, SerialiseObject(*instance, false));
            ApplyPrefabPatches(*instance, localOverrides, graphicsProvider);
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
    EnsurePrefabPatchCache(*root);
    if (!includeRootTransform)
        return root->PrefabHasNonTransformOverrides;
    root->PrefabHasOverrides = root->PrefabHasNonTransformOverrides ||
        PrefabRootTransformDiffers(*root);
    return root->PrefabHasOverrides;
}

bool SceneSerializer::RevertPrefabOverrides(Engine::Core::Object& instance,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    Engine::Core::Object* root = instance.GetPrefabInstanceRoot();
    if (!root || !root->Prefab || root->PrefabSourceSnapshot.empty()) return false;
    try
    {
        const bool result = ApplyObjectState(*root,
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
        SetPrefabSourceSnapshot(*root,
            LoadPrefabDocument(ResolvePrefabPath(path))["object"]);
    }
    catch (...) { return false; }
    root->PrefabOverrideCacheValid = false;
    Engine::Scene::Scene* scene = root->GetScene();
    return !scene || RefreshPrefabInstances(*scene, path, graphicsProvider, root);
}
}
