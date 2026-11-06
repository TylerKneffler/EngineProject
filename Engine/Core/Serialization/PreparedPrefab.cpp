#include "PreparedPrefab.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Engine::Serialization
{
std::string PreparedPrefab::PathKey(const std::string& path)
{
    const auto resolved = Engine::Components::Mesh::ResolveFilePath(path);
    std::string key = std::filesystem::absolute(resolved).lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

std::unique_ptr<PreparedPrefab> PreparedPrefab::Load(const std::string& path,
    const std::shared_ptr<std::atomic_bool>& canceled)
{
    if (canceled && canceled->load()) return nullptr;
    auto result = std::make_unique<PreparedPrefab>();
    result->path = path;
    result->document = SceneSerializer::ReadPrefabFile(path);
    std::vector<std::string> stack{PathKey(path)};
    std::function<void(const JsonValue&)> visit = [&](const JsonValue& node)
    {
        if (canceled && canceled->load()) return;
        if (node.Has("prefab"))
        {
            const auto reference = node["prefab"].AsString();
            const auto key = PathKey(reference);
            if (stack.size() >= 64 || std::find(stack.begin(), stack.end(), key) != stack.end())
                throw std::runtime_error("Cyclic or excessively nested prefab: " + reference);
            auto found = result->dependencies.find(key);
            if (found == result->dependencies.end())
                found = result->dependencies.emplace(key, SceneSerializer::ReadPrefabFile(reference)).first;
            stack.push_back(key);
            visit(found->second["object"]);
            stack.pop_back();
            return;
        }
        const auto& components = node["components"];
        for (size_t i = 0; i < components.ArraySize(); ++i)
        {
            if (canceled && canceled->load()) return;
            const auto& value = components.ArrayAt(i);
            const auto type = value["type"].AsString();
            std::unique_ptr<Engine::Core::Component> component;
            if (type == "Mesh") component = std::make_unique<Engine::Components::Mesh>();
            else if (type == "Material") component = std::make_unique<Engine::Components::Material>();
            else if (type == "Skeleton") component = std::make_unique<Engine::Components::Skeleton>();
            else if (type == "SkinnedMesh") component = std::make_unique<Engine::Components::SkinnedMesh>();
            else if (type == "AnimationManager") component = std::make_unique<Engine::Components::AnimationManager>();
            if (!component) continue;
            component->Deserialize(value);
            if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component.get()))
                mesh->PrepareDeferredGraphicsData();
            result->components[JsonWrite(value)].push_back(std::move(component));
        }
        const auto& children = node["children"];
        for (size_t i = 0; i < children.ArraySize(); ++i) visit(children.ArrayAt(i));
    };
    visit(result->document["object"]);
    if (canceled && canceled->load()) return nullptr;
    return result;
}

Engine::Core::Component* PreparedPrefab::TakeComponent(const JsonValue& value)
{
    if (!current) return nullptr;
    const auto type = value["type"].AsString();
    if (type != "Mesh" && type != "Material" && type != "Skeleton" &&
        type != "SkinnedMesh" && type != "AnimationManager") return nullptr;
    auto found = current->components.find(JsonWrite(value));
    if (found == current->components.end() || found->second.empty()) return nullptr;
    auto component = std::move(found->second.front());
    found->second.pop_front();
    return component.release();
}

const JsonValue* PreparedPrefab::FindDocument(const std::string& path)
{
    if (!current) return nullptr;
    const auto found = current->dependencies.find(PathKey(path));
    return found == current->dependencies.end() || !found->second.IsObject() ? nullptr : &found->second;
}

JsonValue PreparedPrefab::CopyOrConsumeDocument(const JsonValue& value)
{
    if (current)
    {
        if (&value == &current->document) return std::move(current->document);
        for (auto& [path, document] : current->dependencies)
            if (&value == &document) return std::move(document);
    }
    return value;
}

PreparedPrefab::Publication::Publication(PreparedPrefab& prepared) : previous(current)
{ current = &prepared; }
PreparedPrefab::Publication::~Publication() { current = previous; }
}
