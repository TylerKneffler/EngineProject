#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/component.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"
#include <pugixml.hpp>
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <stdexcept>
#include <sstream>
#include <vector>

static bool DeserialiseScene(Scene& scene, const JsonValue& root,
    IGraphicsProvider* graphicsProvider,
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

void SceneSerializer::EnsureBuiltinsRegistered()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    RegisterComponentType<Mesh>("Mesh");
    RegisterComponentType<Material>("Material");
    RegisterComponentType<Camera>("Camera");
}

// ---- Local GLM helpers ------------------------------------------------------
namespace
{

JsonValue J3(float x, float y, float z)
{
    return JsonValue::MakeArray().Push(JsonValue(x)).Push(JsonValue(y)).Push(JsonValue(z));
}

JsonValue JGlm(const glm::vec3& v) { return J3(v.x, v.y, v.z); }

glm::vec3 GlmFrom(const JsonValue& v, glm::vec3 def = {})
{
    if (!v.IsArray() || v.ArraySize() < 3) return def;
    return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
}

bool LooksLikeXml(const std::string& source)
{
    const std::size_t start = source.find_first_not_of(" \t\r\n");
    return start != std::string::npos && source[start] == '<';
}

bool IsStructuralName(const std::string& name)
{
    return name == "Scene" || name == "objects" || name == "components" ||
           name == "children";
}

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ToTitleCopy(std::string value)
{
    if (!value.empty())
        value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    return value;
}

bool SplitTypedName(const std::string& name, std::string& baseName, std::string& typeName)
{
    const std::size_t colon = name.find(':');
    if (colon == std::string::npos)
        return false;
    baseName = name.substr(0, colon);
    typeName = name.substr(colon + 1);
    return true;
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
    std::string baseName;
    std::string typeName;
    const bool isTypedElement = SplitTypedName(name, baseName, typeName);

    if (name == "objects" || name == "children")
    {
        JsonValue array = JsonValue::MakeArray();
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                array.Push(ReadXmlNode(child));
        return array;
    }

    if (name == "components")
    {
        JsonValue array = JsonValue::MakeArray();
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                array.Push(ReadXmlNode(child));
        return array;
    }

    if (isTypedElement && baseName == "components")
    {
        JsonValue component = JsonValue::MakeObject();
        component.Set("type", JsonValue(ToTitleCopy(typeName)));
        for (const pugi::xml_node& child : node.children())
        {
            if (child.type() != pugi::node_element)
                continue;
            component.Set(child.name(), ReadXmlNode(child));
        }
        return component;
    }

    if (!node.first_child())
        return ParseScalarText(node.child_value());

    std::string firstChildName;
    bool allSame = true;
    for (const pugi::xml_node& child : node.children())
    {
        if (child.type() != pugi::node_element)
            continue;
        if (firstChildName.empty())
            firstChildName = child.name();
        else if (child.name() != firstChildName)
        {
            allSame = false;
            break;
        }
    }

    const bool looksLikeOwnedList =
        firstChildName == "item" ||
        firstChildName == "object" ||
        firstChildName == "component" ||
        firstChildName == "components:mesh" ||
        firstChildName == "components:material" ||
        firstChildName == "components:rotate" ||
        firstChildName == name + ".item" ||
        firstChildName == name + ".object" ||
        firstChildName == name + ".component";

    if (allSame && looksLikeOwnedList)
    {
        JsonValue array = JsonValue::MakeArray();
        for (const pugi::xml_node& child : node.children())
            if (child.type() == pugi::node_element)
                array.Push(ReadXmlNode(child));
        return array;
    }

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
void WriteXmlArrayItems(pugi::xml_node parent, const JsonValue& value, const char* itemName)
{
    for (std::size_t i = 0; i < value.ArraySize(); ++i)
    {
        pugi::xml_node child = parent.append_child(itemName);
        WriteXmlNode(child, value.ArrayAt(i));
    }
}

void WriteXmlObjectFields(pugi::xml_node parent, const JsonValue& value, const char* skipKey = nullptr)
{
    for (std::size_t i = 0; i < value.ObjectSize(); ++i)
    {
        const std::string key = value.ObjectKey(i);
        if (skipKey && key == skipKey)
            continue;
        WriteXmlField(parent, key.c_str(), value.ObjectValue(i));
    }
}

void WriteXmlField(pugi::xml_node parent, const char* name, const JsonValue& value)
{
    const std::string parentName = parent.name();
    std::string childName = name ? name : "";
    if ((parentName == "settings" || parentName == "object" ||
        parentName == "transform") && name && *name)
    {
        childName = parentName + "." + name;
    }

    pugi::xml_node child = parent.append_child(childName.c_str());
    if (value.IsArray())
    {
        if (name && std::string(name) == "components")
        {
            for (std::size_t i = 0; i < value.ArraySize(); ++i)
            {
                const JsonValue& component = value.ArrayAt(i);
                const std::string typeName = component["type"].AsString(); // Use capitalized name
                pugi::xml_node typed = child.append_child((std::string("components:") + typeName).c_str());
                WriteXmlObjectFields(typed, component, "type");
            }
            return;
        }

        const char* itemName = "item";
        if (name && std::string(name) == "objects")
            itemName = "object";
        else if (name && std::string(name) == "children")
            itemName = "object";

        WriteXmlArrayItems(child, value, itemName);
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
    root.append_attribute("xmlns:components") = "urn:engine-components";
    if (value.IsObject())
        WriteXmlObjectFields(root, value);
    else
        WriteXmlNode(root, value);

    std::ostringstream stream;
    document.save(stream, "  ");
    return stream.str();
}

bool LoadXmlDocument(Scene& scene, const std::string& source,
    IGraphicsProvider* graphicsProvider,
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

// ---- Serialise a single Object (recursive) ----------------------------------
static JsonValue SerialiseTransform(const Transform& transform)
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("position", JGlm(transform.position));
    node.Set("rotation", JGlm(transform.rotation));
    node.Set("scale", JGlm(transform.scale));
    return node;
}

static void DeserialiseTransform(Transform& transform, const JsonValue& node)
{
    if (!node.IsObject())
        return;
    transform.position = GlmFrom(node["position"]);
    transform.rotation = GlmFrom(node["rotation"]);
    transform.scale = GlmFrom(node["scale"], { 1.f, 1.f, 1.f });
}

static JsonValue SerialiseObject(
    const Object& obj,
    bool serialisePrefabAsReference = true)
{
    JsonValue node = JsonValue::MakeObject();
    if (serialisePrefabAsReference && obj.Prefab)
    {
        node.Set("prefab", JsonValue(obj.Prefab->GetPath()));
        node.Set("transform", SerialiseTransform(obj.transform));
        return node;
    }

    node.Set("name", JsonValue(obj.name));

    // Transform
    node.Set("transform", SerialiseTransform(obj.transform));

    // Components
    JsonValue comps = JsonValue::MakeArray();
    for (const Component* comp : obj.Components)
        comps.Push(comp->Serialize());
    node.Set("components", std::move(comps));

    // Children (recursive)
    JsonValue children = JsonValue::MakeArray();
    for (const Object* child : obj.Children)
        children.Push(SerialiseObject(*child, true));
    node.Set("children", std::move(children));

    return node;
}

static JsonValue SerialiseScene(const Scene& scene)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));

    // Settings
    const SceneSettings& s = scene.settings;
    JsonValue settings = JsonValue::MakeObject();
    settings.Set("showGrid",        JsonValue(s.showGrid));
    settings.Set("gridHalfSize",    JsonValue(s.gridHalfSize));
    settings.Set("gridCellSize",    JsonValue(s.gridCellSize));
    settings.Set("gridOpacity",     JsonValue(s.gridOpacity));
    settings.Set("gridColor",       JGlm(s.gridColor));
    settings.Set("gridOriginColor", JGlm(s.gridOriginColor));
    settings.Set("ambientColor",    JGlm(s.ambientColor));
    root.Set("settings", std::move(settings));

    // Objects — only root-level (no parent)
    JsonValue objects = JsonValue::MakeArray();
    for (const auto& obj : scene.GetObjects())
        if (obj->Parent == nullptr)
            objects.Push(SerialiseObject(*obj));
    root.Set("objects", std::move(objects));

    return root;
}

// ---- Save -------------------------------------------------------------------
std::string SceneSerializer::SaveToString(const Scene& scene)
{
    return WriteXmlDocument(SerialiseScene(scene));
}

bool SceneSerializer::Save(const Scene& scene, const std::string& path)
{
    std::ofstream file(path);
    if (!file) return false;
    file << SaveToString(scene);
    return file.good();
}

static bool DeserialiseScene(Scene& scene, const JsonValue& root,
    IGraphicsProvider* graphicsProvider,
    const std::unordered_map<std::string, SceneSerializer::Factory>& registry)
{
    if (!root.IsObject()) return false;

    int version = root["version"].AsInt();
    if (version != 1) return false;

    // Clear existing scene content
    scene.ClearObjects();

    // Settings
    const JsonValue& settings = root["settings"];
    if (settings.IsObject())
    {
        SceneSettings& s = scene.settings;
        if (settings.Has("showGrid"))      s.showGrid      = settings["showGrid"].AsBool();
        if (settings.Has("gridHalfSize"))  s.gridHalfSize  = settings["gridHalfSize"].AsInt();
        if (settings.Has("gridCellSize"))  s.gridCellSize  = settings["gridCellSize"].AsFloat();
        if (settings.Has("gridOpacity"))   s.gridOpacity   = settings["gridOpacity"].AsFloat();
        if (settings.Has("gridColor"))     s.gridColor     = GlmFrom(settings["gridColor"], s.gridColor);
        if (settings.Has("gridOriginColor")) s.gridOriginColor = GlmFrom(settings["gridOriginColor"], s.gridOriginColor);
        if (settings.Has("ambientColor"))  s.ambientColor  = GlmFrom(settings["ambientColor"], s.ambientColor);
    }

    // Get buffer factory for mesh GPU resource creation
    IGraphicsBufferFactory* bufferFactory = nullptr;
    if (graphicsProvider)
        bufferFactory = graphicsProvider->GetBufferFactory();

    // Deserialise a single Object node, then recurse for children.
    // Uses std::function so it can reference itself (recursive lambda).
    std::function<void(Object&, const JsonValue&)> deserialise
        = [&](Object& obj, const JsonValue& node)
    {
        if (node.Has("name"))
            obj.name = node["name"].AsString();
        if (node.Has("prefab"))
            obj.SetPrefab(node["prefab"].AsString());

        // Transform
        DeserialiseTransform(obj.transform, node["transform"]);

        // Components
        const JsonValue& comps = node["components"];
        for (std::size_t i = 0; i < comps.ArraySize(); ++i)
        {
            const JsonValue& cn  = comps.ArrayAt(i);
            const std::string& t = cn["type"].AsString();

            auto it = registry.find(t);
            if (it == registry.end())
            {
                std::string msg = "[SceneSerializer] Unknown component type '" + t + "' on object '" + obj.name + "' (skipped)\n";
                OutputDebugStringA(msg.c_str());
                continue;
            }

            Component* comp = it->second();        // factory: default-construct
            comp->Owner = &obj;
            comp->Deserialize(cn);
            obj.Components.push_back(comp);

            // Mesh needs GPU resources created immediately after CPU load.
            if (bufferFactory)
                if (Mesh* mesh = dynamic_cast<Mesh*>(comp))
                    mesh->CreateBuffer(bufferFactory);
        }

        // Children (recursive) — added to scene so they participate in loops.
        const JsonValue& children = node["children"];
        for (std::size_t i = 0; i < children.ArraySize(); ++i)
        {
            const JsonValue& childNode = children.ArrayAt(i);
            Object* child = nullptr;
            if (childNode.Has("prefab"))
            {
                child = SceneSerializer::InstantiatePrefab(
                    scene, childNode["prefab"].AsString(), graphicsProvider);
                if (!child)
                    throw std::runtime_error(
                        "Could not instantiate nested prefab '" +
                        childNode["prefab"].AsString() + "'");
                DeserialiseTransform(child->transform, childNode["transform"]);
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
            Object* object = SceneSerializer::InstantiatePrefab(
                scene, objectNode["prefab"].AsString(), graphicsProvider);
            if (!object)
                throw std::runtime_error(
                    "Could not instantiate prefab '" +
                    objectNode["prefab"].AsString() + "'");
            DeserialiseTransform(object->transform, objectNode["transform"]);
        }
        else
        {
            Object* object = scene.AddObject();
            deserialise(*object, objectNode);
        }
    }

    return true;
}

// ---- Load -------------------------------------------------------------------
bool SceneSerializer::LoadFromString(Scene& scene, const std::string& source,
    IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try { return DeserialiseScene(scene, JsonParse(source), graphicsProvider, GetRegistry()); }
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

bool SceneSerializer::Load(Scene& scene, const std::string& path, IGraphicsProvider* graphicsProvider)
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

bool SceneSerializer::SavePrefab(const Object& object, const std::string& path)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("type", JsonValue(std::string("prefab")));
    // A prefab file owns the complete expanded object definition. If this
    // object is already an instance, do not write a self-referencing link.
    root.Set("object", SerialiseObject(object, false));

    std::ofstream file(path);
    if (!file)
        return false;
    file << JsonWrite(root);
    return file.good();
}

Object* SceneSerializer::InstantiatePrefab(
    Scene& scene,
    const std::string& path,
    IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    Object* rootObject = nullptr;
    static thread_local std::vector<std::string> prefabStack;
    std::error_code absoluteError;
    std::filesystem::path identity = std::filesystem::absolute(path, absoluteError);
    if (absoluteError)
        identity = std::filesystem::path(path);
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
        const JsonValue root = JsonParseFile(path);
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            root["type"].AsString() != "prefab" || !root["object"].IsObject())
            return nullptr;

        IGraphicsBufferFactory* bufferFactory = graphicsProvider
            ? graphicsProvider->GetBufferFactory()
            : nullptr;

        std::function<void(Object&, const JsonValue&)> instantiate =
            [&](Object& object, const JsonValue& node)
        {
            if (node.Has("name"))
                object.name = node["name"].AsString();

            DeserialiseTransform(object.transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t i = 0; i < components.ArraySize(); ++i)
            {
                const JsonValue& componentNode = components.ArrayAt(i);
                const std::string type = componentNode["type"].AsString();
                auto factory = GetRegistry().find(type);
                if (factory == GetRegistry().end())
                    continue;

                Component* component = factory->second();
                component->Owner = &object;
                component->Deserialize(componentNode);
                object.Components.push_back(component);
                if (bufferFactory)
                    if (Mesh* mesh = dynamic_cast<Mesh*>(component))
                        mesh->CreateBuffer(bufferFactory);
            }

            const JsonValue& children = node["children"];
            for (std::size_t i = 0; i < children.ArraySize(); ++i)
            {
                const JsonValue& childNode = children.ArrayAt(i);
                Object* child = nullptr;
                if (childNode.Has("prefab"))
                {
                    child = InstantiatePrefab(
                        scene, childNode["prefab"].AsString(), graphicsProvider);
                    if (!child)
                        throw std::runtime_error(
                            "Could not instantiate nested prefab '" +
                            childNode["prefab"].AsString() + "'");
                    DeserialiseTransform(child->transform, childNode["transform"]);
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
    Scene& scene,
    const std::string& path,
    IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    try
    {
        const JsonValue root = JsonParseFile(path);
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

        const std::string targetKey = identity(path);
        std::vector<Object*> instances;
        for (const auto& candidate : scene.GetObjects())
            if (candidate->Prefab &&
                identity(candidate->Prefab->GetPath()) == targetKey)
                instances.push_back(candidate.get());

        IGraphicsBufferFactory* bufferFactory = graphicsProvider
            ? graphicsProvider->GetBufferFactory()
            : nullptr;
        std::function<void(Object&, const JsonValue&)> populate =
            [&](Object& object, const JsonValue& node)
        {
            object.name = node.Has("name")
                ? node["name"].AsString()
                : std::string("Object");
            DeserialiseTransform(object.transform, node["transform"]);

            const JsonValue& components = node["components"];
            for (std::size_t index = 0; index < components.ArraySize(); ++index)
            {
                const JsonValue& componentNode = components.ArrayAt(index);
                const std::string type = componentNode["type"].AsString();
                auto factory = GetRegistry().find(type);
                if (factory == GetRegistry().end())
                    continue;
                Component* component = factory->second();
                component->Owner = &object;
                component->Deserialize(componentNode);
                object.Components.push_back(component);
                if (bufferFactory)
                    if (Mesh* mesh = dynamic_cast<Mesh*>(component))
                        mesh->CreateBuffer(bufferFactory);
            }

            const JsonValue& children = node["children"];
            for (std::size_t index = 0; index < children.ArraySize(); ++index)
            {
                const JsonValue& childNode = children.ArrayAt(index);
                Object* child = nullptr;
                if (childNode.Has("prefab"))
                {
                    child = InstantiatePrefab(
                        scene, childNode["prefab"].AsString(), graphicsProvider);
                    if (!child)
                        throw std::runtime_error(
                            "Could not refresh nested prefab '" +
                            childNode["prefab"].AsString() + "'");
                    DeserialiseTransform(child->transform, childNode["transform"]);
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

        for (Object* instance : instances)
        {
            const Transform placement = instance->transform;
            const auto prefab = instance->Prefab;
            const std::vector<Object*> oldChildren = instance->Children;
            for (Object* child : oldChildren)
                scene.RemoveObject(child);
            instance->Children.clear();
            for (Component* component : instance->Components)
                delete component;
            instance->Components.clear();

            populate(*instance, root["object"]);
            instance->transform = placement;
            instance->Prefab = prefab;
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
