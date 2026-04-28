#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Core/component.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"
#include <fstream>
#include <functional>
#include <stdexcept>

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

    Register("Mesh",     []() -> Component* { return new Mesh(); });
    Register("Material", []() -> Component* { return new Material(); });
    Register("Camera",   []() -> Component* { return new Camera(); });
}

// ---- Local GLM / XMFLOAT3 helpers ------------------------------------------
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

JsonValue JXm(const DirectX::XMFLOAT3& v) { return J3(v.x, v.y, v.z); }

DirectX::XMFLOAT3 XmFrom(const JsonValue& v, DirectX::XMFLOAT3 def = {})
{
    if (!v.IsArray() || v.ArraySize() < 3) return def;
    return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
}

} // namespace

// ---- Serialise a single Object (recursive) ----------------------------------
static JsonValue SerialiseObject(const Object& obj)
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("name", JsonValue(obj.name));

    // Transform
    JsonValue tf = JsonValue::MakeObject();
    tf.Set("position", JGlm(obj.transform.position));
    tf.Set("rotation", JGlm(obj.transform.rotation));
    tf.Set("scale",    JGlm(obj.transform.scale));
    node.Set("transform", std::move(tf));

    // Components
    JsonValue comps = JsonValue::MakeArray();
    for (const Component* comp : obj.Components)
        comps.Push(comp->Serialize());
    node.Set("components", std::move(comps));

    // Children (recursive)
    JsonValue children = JsonValue::MakeArray();
    for (const Object* child : obj.Children)
        children.Push(SerialiseObject(*child));
    node.Set("children", std::move(children));

    return node;
}

// ---- Save -------------------------------------------------------------------
bool SceneSerializer::Save(const Scene& scene, const std::string& path)
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

    std::ofstream file(path);
    if (!file) return false;
    file << JsonWrite(root);
    return file.good();
}

// ---- Load -------------------------------------------------------------------
bool SceneSerializer::Load(Scene& scene, const std::string& path, IGraphicsProvider* graphicsProvider)
{
    EnsureBuiltinsRegistered();
    auto& registry = GetRegistry();

    JsonValue root;
    try { root = JsonParseFile(path); }
    catch (const std::exception&) { return false; }

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

        // Transform
        const JsonValue& tf = node["transform"];
        if (tf.IsObject())
        {
            obj.transform.position = GlmFrom(tf["position"]);
            obj.transform.rotation = GlmFrom(tf["rotation"]);
            obj.transform.scale    = GlmFrom(tf["scale"], { 1.f, 1.f, 1.f });
        }

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
            Object* child = scene.AddObject();
            child->Parent = &obj;
            obj.Children.push_back(child);
            deserialise(*child, children.ArrayAt(i));
        }
    };

    // Top-level objects
    const JsonValue& objects = root["objects"];
    for (std::size_t i = 0; i < objects.ArraySize(); ++i)
    {
        Object* obj = scene.AddObject();
        deserialise(*obj, objects.ArrayAt(i));
    }

    return true;
}
