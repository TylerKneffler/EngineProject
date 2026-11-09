#pragma once

#include "Core/Model/ImportedModel.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Editor::ModelAssetWriterSupport
{
namespace fs = std::filesystem;

std::string SafeName(std::string name, const std::string& fallback);
fs::path UniqueDirectory(const fs::path& assets, const std::string& base);

template<class T>
inline bool CompactOffsets(
    const std::vector<T>& source,
    const std::vector<uint32_t>& representatives,
    const std::vector<uint32_t>& indices,
    std::vector<T>& result)
{
    if (source.empty()) return true;
    if (source.size() != indices.size()) return false;
    for (size_t old = 0; old < source.size(); ++old)
        if (std::memcmp(&source[old], &source[representatives[indices[old]]],
                sizeof(T)) != 0)
            return false;
    result.reserve(representatives.size());
    for (uint32_t representative : representatives)
        result.push_back(source[representative]);
    return true;
}

struct IndexedPrimitiveAsset
{
    std::vector<Engine::Model::AnimationVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Engine::Model::MorphTarget> morphTargets;
};

std::optional<IndexedPrimitiveAsset> IndexPmxPrimitive(
    const Engine::Model::ImportedPrimitive& primitive);

glm::vec3 QuaternionEuler(const glm::quat& q);
Engine::Core::Object* AddChild(Engine::Scene::Scene& scene,
    Engine::Core::Object* parent,
    const std::string& name);
}
