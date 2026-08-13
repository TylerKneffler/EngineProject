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

namespace Engine::Serialization::Detail
{
// ---- Serialise a single Engine::Core::Object (recursive) ----------------------------------
JsonValue SceneObjectBehavior::SerializeTransform(const Engine::Components::Transform& transform)
{
    return transform.Serialize();
}

void SceneObjectBehavior::DeserializeTransform(Engine::Components::Transform& transform, const JsonValue& node)
{
    if (!node.IsObject())
        return;
    transform.Deserialize(node);
}

void SceneObjectBehavior::SetPrefabSourceSnapshot(Engine::Core::Object& object, const JsonValue& source)
{
    object.PrefabSourceSnapshot = JsonWrite(source);
    Engine::Components::Transform sourceTransform;
    DeserializeTransform(sourceTransform, source["transform"]);
    object.PrefabSourcePosition = sourceTransform.position;
    object.PrefabSourceRotation = sourceTransform.rotation;
    object.PrefabSourceScale = sourceTransform.scale;
    object.PrefabSourceTransformValid = true;
    object.PrefabOverrideCacheValid = false;
}

bool SceneObjectBehavior::PrefabRootTransformDiffers(const Engine::Core::Object& instance)
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

JsonValue SceneObjectBehavior::SerializeBakedMaterialOverrides(const Engine::Core::Object& root)
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

void SceneObjectBehavior::ApplyBakedMaterialOverrides(Engine::Core::Object& root, const JsonValue& node,
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

JsonValue SceneObjectBehavior::SerializeObject(
    const Engine::Core::Object& obj,
    bool serialisePrefabAsReference)
{
    JsonValue node = JsonValue::MakeObject();
    if (serialisePrefabAsReference && obj.Prefab)
    {
        node.Set("prefab", JsonValue(obj.Prefab->GetPath()));
        node.Set("transform", SerializeTransform(obj.transform));
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
        const JsonValue overrides = SerializeBakedMaterialOverrides(obj);
        if (overrides.ArraySize() > 0)
            node.Set("bakedMaterialOverrides", overrides);
        return node;
    }

    node.Set("name", JsonValue(obj.name));
    node.Set("enabled", JsonValue(obj.enabled));

    // Engine::Components::Transform
    node.Set("transform", SerializeTransform(obj.transform));

    // Components
    JsonValue comps = JsonValue::MakeArray();
    for (const Engine::Core::Component* comp : obj.Components)
        comps.Push(comp->Serialize());
    node.Set("components", std::move(comps));

    // Children (recursive)
    JsonValue children = JsonValue::MakeArray();
    for (const Engine::Core::Object* child : obj.Children)
        children.Push(SerializeObject(*child, true));
    node.Set("children", std::move(children));

    return node;
}

bool SceneObjectBehavior::JsonEquals(const JsonValue& left, const JsonValue& right)
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

JsonValue SceneObjectBehavior::PathWith(const JsonValue& path, const std::string& key)
{
    JsonValue result = path;
    JsonValue segment = JsonValue::MakeObject();
    segment.Set("key", JsonValue(key));
    result.Push(std::move(segment));
    return result;
}

JsonValue SceneObjectBehavior::PathWith(const JsonValue& path, size_t index)
{
    JsonValue result = path;
    JsonValue segment = JsonValue::MakeObject();
    segment.Set("index", JsonValue(static_cast<int>(index)));
    result.Push(std::move(segment));
    return result;
}

void SceneObjectBehavior::AddPatch(JsonValue& patches, const JsonValue& path,
    const JsonValue* value)
{
    JsonValue patch = JsonValue::MakeObject();
    patch.Set("path", path);
    if (value) patch.Set("value", *value);
    else patch.Set("remove", JsonValue(true));
    patches.Push(std::move(patch));
}

void SceneObjectBehavior::DiffJson(const JsonValue& baseline, const JsonValue& current,
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

JsonValue SceneObjectBehavior::BuildPrefabPatches(const Engine::Core::Object& instance,
    bool includeRootTransform)
{
    JsonValue patches = JsonValue::MakeArray();
    if (instance.PrefabSourceSnapshot.empty()) return patches;
    try
    {
        const JsonValue baseline = JsonParse(instance.PrefabSourceSnapshot);
        const JsonValue current = SerializeObject(instance, false);
        DiffJson(baseline, current, JsonValue::MakeArray(), patches,
            !includeRootTransform);
    }
    catch (...) {}
    return patches;
}

void SceneObjectBehavior::EnsurePrefabPatchCache(const Engine::Core::Object& instance)
{
    if (instance.PrefabOverrideCacheValid) return;
    const JsonValue nonTransform = BuildPrefabPatches(instance, false);
    instance.PrefabOverridePatchSnapshot = JsonWrite(nonTransform);
    instance.PrefabHasNonTransformOverrides = nonTransform.ArraySize() > 0;
    instance.PrefabHasOverrides = instance.PrefabHasNonTransformOverrides ||
        PrefabRootTransformDiffers(instance);
    instance.PrefabOverrideCacheValid = true;
}

bool SceneObjectBehavior::ApplyPatch(JsonValue& target, const JsonValue& patch)
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

bool SceneObjectBehavior::ApplyObjectState(Engine::Core::Object& object, const JsonValue& node,
    Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!node.IsObject()) return false;
    if (node.Has("name")) object.name = node["name"].AsString();
    object.enabled = !node.Has("enabled") || node["enabled"].AsBool();
    DeserializeTransform(object.transform, node["transform"]);
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

bool SceneObjectBehavior::ApplyPrefabPatches(Engine::Core::Object& instance, const JsonValue& patches,
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

JsonValue SceneObjectBehavior::SerializeScene(const Engine::Scene::Scene& scene)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));

    // Settings
    root.Set("settings", Engine::Scene::SerializeSceneSettings(scene.settings));

    // Objects — only root-level (no parent)
    JsonValue objects = JsonValue::MakeArray();
    for (const auto& obj : scene.GetObjects())
        if (obj->Parent == nullptr)
            objects.Push(SerializeObject(*obj));
    root.Set("objects", std::move(objects));

    return root;
}

}
