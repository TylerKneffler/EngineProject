#pragma once

#include "Core/Serialization/Json.h"

#include <cstddef>
#include <string>
#include <vector>

// A prefab carries all of its detail levels. Each Mesh component lists the
// assets that replace it at LOD 1..N (Mesh.lodMeshes; an empty entry hides the
// part at that level) and each Material may list replacement materials
// (Material.lodMaterials; an empty entry keeps the base material).
//
// Any system that streams prefab paths (terrain object layers, far-object
// cells) addresses a level as a virtual path, "Trees/Oak.prefab#lod2". The
// prefab loader builds that document from the base prefab on demand, so a
// single asset replaces the former Oak_LOD0/Oak_LOD1/... prefab files.
namespace Engine::Serialization::PrefabLod
{
constexpr const char* Marker = "#lod";
// Matches TerrainObjectLayer::MaximumObjectLods.
constexpr size_t MaximumLevels = 16;

struct Variant
{
    std::string base;
    size_t lod = 0;
};

// "a.prefab#lod2" -> {"a.prefab", 2}; a plain path is level 0.
Variant Split(const std::string& path);
// The base path for level 0, otherwise "base#lodN".
std::string Path(const std::string& base, size_t lod);
// The path names a prefab, either directly or as a level of one.
bool IsPrefab(const std::string& path);

// 1 + the longest Mesh.lodMeshes list in the document's own hierarchy.
size_t CountLevels(const JsonValue& document);
// Rewrite a prefab document in place so it describes level `lod`.
void Apply(JsonValue& document, size_t lod);

// Component serialization of Mesh.lodMeshes / Material.lodMaterials. Empty
// entries round-trip (XML may read them back as null), trailing empties are
// dropped, and at most MaximumLevels - 1 entries are kept.
JsonValue WritePaths(const std::vector<std::string>& paths);
std::vector<std::string> ReadPaths(const JsonValue& value);

// Level count of a prefab file. Cached per path and revalidated against the
// file's timestamp at most once a second; safe from any thread. Unreadable
// files and non-prefab paths count one level.
size_t LevelCount(const std::string& prefabPath);
// {base, base#lod1, ...} for a prefab with levels, otherwise {path}.
std::vector<std::string> Chain(const std::string& prefabPath);
}
