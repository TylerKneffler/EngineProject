#include "Core/Serialization/PrefabLod.h"

#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Serialization/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>

namespace Engine::Serialization::PrefabLod
{
namespace
{
std::string LowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::string StringAt(const JsonValue& array, size_t index)
{
    if (!array.IsArray() || index >= array.ArraySize()) return {};
    const auto& value = array.ArrayAt(index);
    return value.IsString() ? value.AsString() : std::string{};
}

size_t CountNodeLevels(const JsonValue& node)
{
    // A nested prefab reference keeps its own levels; it is not expanded.
    if (!node.IsObject() || node.Has("prefab")) return 1;
    size_t levels = 1;
    const auto& components = node["components"];
    for (size_t index = 0; index < components.ArraySize(); ++index)
    {
        const auto& component = components.ArrayAt(index);
        if (component["type"].IsString() && component["type"].AsString() == "Mesh" &&
            component["lodMeshes"].IsArray())
            levels = std::max(levels, component["lodMeshes"].ArraySize() + 1);
    }
    const auto& children = node["children"];
    for (size_t index = 0; index < children.ArraySize(); ++index)
        levels = std::max(levels, CountNodeLevels(children.ArrayAt(index)));
    return std::min(levels, MaximumLevels);
}

void ApplyNode(JsonValue& node, size_t lod)
{
    if (!node.IsObject() || node.Has("prefab")) return;
    const JsonValue& readable = node;
    if (readable["components"].IsArray())
    {
        const JsonValue& source = readable["components"];
        // A part with a LOD list but no entry for this level is absent.
        bool hidden = false;
        for (size_t index = 0; index < source.ArraySize(); ++index)
        {
            const auto& component = source.ArrayAt(index);
            if (component["type"].IsString() && component["type"].AsString() == "Mesh" &&
                component["lodMeshes"].IsArray() && component["lodMeshes"].ArraySize() > 0 &&
                StringAt(component["lodMeshes"], lod - 1).empty())
                hidden = true;
        }
        JsonValue rewritten = JsonValue::MakeArray(source.ArraySize());
        for (size_t index = 0; index < source.ArraySize(); ++index)
        {
            // Read through const references: the mutable operator[] inserts.
            const JsonValue& original = source.ArrayAt(index);
            const std::string type = original["type"].IsString()
                ? original["type"].AsString() : std::string{};
            JsonValue component = original;
            if (type == "Mesh")
            {
                if (hidden) continue;
                const std::string file = StringAt(original["lodMeshes"], lod - 1);
                if (!file.empty()) component.Set("file", JsonValue(file));
                component.Erase("lodMeshes");
            }
            else if (type == "Material")
            {
                if (hidden) continue;
                const std::string material = StringAt(original["lodMaterials"], lod - 1);
                if (!material.empty())
                {
                    // Inline fields saved with the base material would
                    // override the level's own asset; start from the asset.
                    JsonValue replacement = JsonValue::MakeObject();
                    replacement.Set("type", JsonValue("Material"));
                    if (original.Has("enabled")) replacement.Set("enabled", original["enabled"]);
                    replacement.Set("materialAsset", JsonValue(material));
                    component = std::move(replacement);
                }
                else component.Erase("lodMaterials");
            }
            rewritten.Push(std::move(component));
        }
        node.Set("components", std::move(rewritten));
    }
    if (node.Has("children") && node["children"].IsArray())
    {
        auto& children = node["children"];
        for (size_t index = 0; index < children.ArraySize(); ++index)
            ApplyNode(children.ArrayAt(index), lod);
    }
}
}

Variant Split(const std::string& path)
{
    Variant result{path, 0};
    const auto marker = path.rfind(Marker);
    if (marker == std::string::npos) return result;
    const auto digits = marker + std::char_traits<char>::length(Marker);
    if (digits >= path.size()) return result;
    size_t lod = 0;
    for (size_t index = digits; index < path.size(); ++index)
    {
        if (!std::isdigit(static_cast<unsigned char>(path[index]))) return result;
        lod = lod * 10 + static_cast<size_t>(path[index] - '0');
        if (lod >= MaximumLevels) return result;
    }
    result.base = path.substr(0, marker);
    result.lod = lod;
    return result;
}

std::string Path(const std::string& base, size_t lod)
{
    const auto plain = Split(base).base;
    return lod == 0 ? plain : plain + Marker + std::to_string(lod);
}

bool IsPrefab(const std::string& path)
{
    return !path.empty() && LowerExtension(Split(path).base) == ".prefab";
}

size_t CountLevels(const JsonValue& document)
{
    return CountNodeLevels(document["object"]);
}

void Apply(JsonValue& document, size_t lod)
{
    if (lod == 0 || !document.IsObject() || !document.Has("object")) return;
    ApplyNode(document["object"], lod);
}

JsonValue WritePaths(const std::vector<std::string>& paths)
{
    JsonValue array = JsonValue::MakeArray(paths.size());
    for (const auto& path : paths) array.Push(JsonValue(path));
    return array;
}

std::vector<std::string> ReadPaths(const JsonValue& value)
{
    std::vector<std::string> paths;
    if (!value.IsArray()) return paths;
    for (size_t index = 0; index < std::min(value.ArraySize(), MaximumLevels - 1); ++index)
        paths.push_back(StringAt(value, index));
    while (!paths.empty() && paths.back().empty()) paths.pop_back();
    return paths;
}

size_t LevelCount(const std::string& prefabPath)
{
    const auto base = Split(prefabPath).base;
    if (LowerExtension(base) != ".prefab") return 1;
    struct Entry
    {
        std::filesystem::file_time_type modified{};
        std::chrono::steady_clock::time_point checked{};
        size_t levels = 1;
        bool valid = false;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex);
        const auto found = cache.find(base);
        if (found != cache.end() && found->second.valid &&
            now - found->second.checked < std::chrono::seconds(1))
            return found->second.levels;
    }
    std::error_code error;
    const auto resolved = Engine::Components::Mesh::ResolveFilePath(base);
    const auto modified = std::filesystem::last_write_time(resolved, error);
    {
        std::lock_guard lock(mutex);
        auto& entry = cache[base];
        if (error)
        {
            entry = Entry{{}, now, 1, true};
            return 1;
        }
        if (entry.valid && entry.modified == modified)
        {
            entry.checked = now;
            return entry.levels;
        }
    }
    size_t levels = 1;
    try { levels = CountLevels(SceneSerializer::ReadPrefabFile(base)); }
    catch (...) { levels = 1; }
    std::lock_guard lock(mutex);
    cache[base] = Entry{modified, now, levels, true};
    return levels;
}

std::vector<std::string> Chain(const std::string& prefabPath)
{
    const auto variant = Split(prefabPath);
    // A path that already names one level is that representation only.
    if (variant.lod != 0) return {prefabPath};
    const size_t levels = LevelCount(prefabPath);
    std::vector<std::string> chain;
    chain.reserve(levels);
    for (size_t lod = 0; lod < levels; ++lod) chain.push_back(Path(prefabPath, lod));
    return chain;
}
}
