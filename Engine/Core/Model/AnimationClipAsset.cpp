#include "AnimationClipAsset.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Object.h"
#include "Core/Serialization/Json.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <glm/gtc/quaternion.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Model
{
namespace
{
Engine::Serialization::JsonValue WriteVector(const glm::vec3& value)
{
    return Engine::Serialization::JsonValue::MakeArray()
        .Push(Engine::Serialization::JsonValue(value.x))
        .Push(Engine::Serialization::JsonValue(value.y))
        .Push(Engine::Serialization::JsonValue(value.z));
}

glm::vec3 ReadVector(const Engine::Serialization::JsonValue& value,
    const glm::vec3& fallback)
{
    if (value.ArraySize() != 3) return fallback;
    return { value.ArrayAt(0).AsFloat(), value.ArrayAt(1).AsFloat(),
        value.ArrayAt(2).AsFloat() };
}

void ApplyFirstKeyPose(const AnimationClip& clip,
    std::vector<AnimationClipAsset::BaseNode>& nodes)
{
    std::unordered_map<std::string, size_t> byPath;
    for (size_t index = 0; index < nodes.size(); ++index)
        byPath.emplace(nodes[index].path, index);
    for (const auto& channel : clip.channels)
    {
        if (channel.times.empty() || channel.valueWidth == 0) continue;
        const size_t start = channel.interpolation ==
            AnimationChannel::Interpolation::CubicSpline
            ? channel.valueWidth : 0;
        if (channel.values.size() < start + channel.valueWidth) continue;
        const auto found = byPath.find(channel.targetPath);
        if (found == byPath.end()) continue;
        auto& node = nodes[found->second];
        const auto& values = channel.values;
        if (channel.path == AnimationChannel::Path::Translation &&
            channel.valueWidth >= 3)
            node.position = { values[start], values[start + 1],
                values[start + 2] };
        else if (channel.path == AnimationChannel::Path::Scale &&
            channel.valueWidth >= 3)
            node.scale = { values[start], values[start + 1],
                values[start + 2] };
        else if (channel.path == AnimationChannel::Path::Rotation &&
            channel.valueWidth >= 4)
        {
            glm::quat rotation(values[start + 3], values[start],
                values[start + 1], values[start + 2]);
            if (glm::length(rotation) < .00001f) continue;
            rotation = glm::normalize(rotation);
            const float x = rotation.x, y = rotation.y, z = rotation.z;
            const float w = rotation.w;
            node.rotation = {
                std::atan2(2.f * (w * x + y * z),
                    1.f - 2.f * (x * x + y * y)),
                std::asin(std::clamp(2.f * (w * y - z * x), -1.f, 1.f)),
                std::atan2(2.f * (w * z + x * y),
                    1.f - 2.f * (y * y + z * z))
            };
        }
    }
}
}

std::vector<AnimationClipAsset::BaseNode>
AnimationClipAsset::CaptureBaseSkeleton(
    const Engine::Components::Model& model)
{
    std::vector<BaseNode> result;
    for (auto* object : model.ResolveNodes())
    {
        if (!object || !model.Owner) continue;
        std::vector<std::string> names;
        for (auto* current = object; current && current != model.Owner;
            current = current->Parent)
            names.push_back(current->name);
        std::string path = names.empty() ? "." : "";
        for (auto it = names.rbegin(); it != names.rend(); ++it)
        {
            if (!path.empty()) path += '/';
            path += *it;
        }
        if (path.empty()) continue;
        BaseNode node;
        node.path = path;
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            node.parent = path.substr(0, slash);
        node.position = object->transform.position;
        node.rotation = object->transform.rotation;
        node.scale = object->transform.scale;
        result.push_back(std::move(node));
    }
    return result;
}

std::optional<AnimationClipAsset> AnimationClipAsset::Load(
    const std::string& path)
{
    try
    {
        const auto root = Engine::Serialization::JsonParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (!root.IsObject()) return std::nullopt;
        const auto& source = root.Has("clip") ? root["clip"] : root;
        if (!source.IsObject() || !source["name"].IsString() ||
            !source["channels"].IsArray()) return std::nullopt;
        AnimationClipAsset asset;
        asset.clip.Deserialize(source);
        asset.previewPrefab = root["previewPrefab"].AsString();
        asset.previewRig = static_cast<size_t>(
            std::max(0, root["previewRig"].AsInt()));
        for (size_t index = 0; index < root["baseSkeleton"].ArraySize(); ++index)
        {
            const auto& node = root["baseSkeleton"].ArrayAt(index);
            if (!node["path"].IsString()) continue;
            BaseNode base;
            base.path = node["path"].AsString();
            base.parent = node["parent"].AsString();
            base.position = ReadVector(node["position"], {});
            base.rotation = ReadVector(node["rotation"], {});
            base.scale = ReadVector(node["scale"], glm::vec3(1.f));
            asset.baseSkeleton.push_back(std::move(base));
        }
        return asset;
    }
    catch (...) { return std::nullopt; }
}

bool AnimationClipAsset::Save(const std::string& path) const
{
    auto root = Engine::Serialization::JsonValue::MakeObject()
        .Set("version", Engine::Serialization::JsonValue(1))
        .Set("previewPrefab", Engine::Serialization::JsonValue(previewPrefab))
        .Set("previewRig", Engine::Serialization::JsonValue(
            static_cast<int>(previewRig)))
        .Set("clip", clip.Serialize());
    auto baseNodes = baseSkeleton;
    ApplyFirstKeyPose(clip, baseNodes);
    auto nodes = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& node : baseNodes)
        nodes.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("path", Engine::Serialization::JsonValue(node.path))
            .Set("parent", Engine::Serialization::JsonValue(node.parent))
            .Set("position", WriteVector(node.position))
            .Set("rotation", WriteVector(node.rotation))
            .Set("scale", WriteVector(node.scale)));
    root.Set("baseSkeleton", std::move(nodes));
    const std::filesystem::path target =
        Engine::Components::Mesh::ResolveFilePath(path);
    std::error_code error;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const auto temporary = std::filesystem::path(target.string() + ".tmp." +
        std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << Engine::Serialization::JsonWrite(root) << '\n';
        if (!file)
        { std::filesystem::remove(temporary, error); return false; }
    }
    bool replaced = false;
#ifdef _WIN32
    replaced = std::filesystem::exists(target)
        ? ReplaceFileW(target.wstring().c_str(), temporary.wstring().c_str(),
            nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0
        : MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
            MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, target, error);
    replaced = !error;
#endif
    if (!replaced) std::filesystem::remove(temporary, error);
    return replaced;
}
}
