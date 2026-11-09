#include "ModelAssetWriterSupport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace Engine::Editor::ModelAssetWriterSupport
{
namespace fs = std::filesystem;

std::string SafeName(std::string name, const std::string& fallback)
{
    if (name.empty()) name = fallback;
    for (char& c : name)
    {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name.empty() ? fallback : name;
}

fs::path UniqueDirectory(const fs::path& assets, const std::string& base)
{
    fs::path result = assets / base;
    for (unsigned suffix = 2; fs::exists(result); ++suffix)
        result = assets / (base + " " + std::to_string(suffix));
    return result;
}

std::optional<IndexedPrimitiveAsset> IndexPmxPrimitive(
    const Engine::Model::ImportedPrimitive& primitive)
{
    if (primitive.vertices.size() < 6 || primitive.vertices.size() % 3)
        return std::nullopt;
    IndexedPrimitiveAsset result;
    result.vertices.reserve(primitive.vertices.size());
    result.indices.reserve(primitive.vertices.size());
    std::vector<uint32_t> representatives;
    std::unordered_map<std::string, uint32_t> byVertex;
    byVertex.reserve(primitive.vertices.size());
    for (size_t old = 0; old < primitive.vertices.size(); ++old)
    {
        const auto& vertex = primitive.vertices[old];
        std::string key(reinterpret_cast<const char*>(&vertex), sizeof(vertex));
        const auto [found, inserted] = byVertex.emplace(std::move(key),
            static_cast<uint32_t>(result.vertices.size()));
        if (inserted)
        {
            result.vertices.push_back(vertex);
            representatives.push_back(static_cast<uint32_t>(old));
        }
        result.indices.push_back(found->second);
    }
    if (result.vertices.size() == primitive.vertices.size())
        return std::nullopt;
    for (const auto& target : primitive.morphTargets)
    {
        Engine::Model::MorphTarget compact;
        compact.name = target.name;
        if (!CompactOffsets(target.positions, representatives, result.indices,
                compact.positions) ||
            !CompactOffsets(target.normals, representatives, result.indices,
                compact.normals) ||
            !CompactOffsets(target.tangents, representatives, result.indices,
                compact.tangents) ||
            !CompactOffsets(target.uv, representatives, result.indices,
                compact.uv))
            return std::nullopt;
        for (size_t channel = 0; channel < compact.uvChannels.size(); ++channel)
            if (!CompactOffsets(target.uvChannels[channel], representatives,
                    result.indices, compact.uvChannels[channel]))
                return std::nullopt;
        result.morphTargets.push_back(std::move(compact));
    }
    return result;
}

glm::vec3 QuaternionEuler(const glm::quat& q)
{
    const float sinX = 2.f * (q.w * q.x + q.y * q.z);
    const float cosX = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float sinY = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
    const float sinZ = 2.f * (q.w * q.z + q.x * q.y);
    const float cosZ = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return { std::atan2(sinX, cosX), std::asin(sinY), std::atan2(sinZ, cosZ) };
}

Engine::Core::Object* AddChild(Engine::Scene::Scene& scene,
    Engine::Core::Object* parent,
    const std::string& name)
{
    Engine::Core::Object* child = scene.AddObject(name);
    scene.SetParentLocal(child, parent);
    return child;
}
}
