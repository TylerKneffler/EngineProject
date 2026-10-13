#include "PoseAsset.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Serialization/Json.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Model
{
namespace
{
using Engine::Serialization::JsonValue;

JsonValue WriteVector(const glm::vec3& value)
{
    return JsonValue::MakeArray().Push(JsonValue(value.x))
        .Push(JsonValue(value.y)).Push(JsonValue(value.z));
}

bool ReadVector(const JsonValue& value, glm::vec3& result)
{
    if (value.ArraySize() != 3) return false;
    for (size_t i = 0; i < 3; ++i)
    {
        if (!value.ArrayAt(i).IsNumber()) return false;
        result[static_cast<int>(i)] = value.ArrayAt(i).AsFloat();
    }
    return true;
}

bool ValidPose(const PoseAsset& asset)
{
    if (asset.name.empty() || asset.joints.empty()) return false;
    std::unordered_set<std::string> paths;
    for (const auto& joint : asset.joints)
    {
        if (joint.path.empty() || !paths.insert(joint.path).second ||
            !std::isfinite(joint.rotation.w) ||
            !std::isfinite(joint.rotation.x) ||
            !std::isfinite(joint.rotation.y) ||
            !std::isfinite(joint.rotation.z) ||
            glm::length(joint.rotation) < 1e-6f)
            return false;
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(joint.position[axis]) ||
                !std::isfinite(joint.scale[axis])) return false;
    }
    return true;
}
}

bool PoseAsset::Save(const std::string& path) const
{
    const std::filesystem::path requested(path);
    if (requested.extension() != ".pose" || !ValidPose(*this)) return false;
    auto root = JsonValue::MakeObject()
        .Set("version", JsonValue(1))
        .Set("name", JsonValue(name));
    auto bones = JsonValue::MakeArray();
    for (const auto& joint : joints)
    {
        auto rotation = JsonValue::MakeArray()
            .Push(JsonValue(joint.rotation.w))
            .Push(JsonValue(joint.rotation.x))
            .Push(JsonValue(joint.rotation.y))
            .Push(JsonValue(joint.rotation.z));
        bones.Push(JsonValue::MakeObject()
            .Set("path", JsonValue(joint.path))
            .Set("position", WriteVector(joint.position))
            .Set("rotation", std::move(rotation))
            .Set("scale", WriteVector(joint.scale)));
    }
    root.Set("joints", std::move(bones));
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
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        const std::string bytes = Engine::Serialization::AssetWriteBinary(root);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output)
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

std::optional<PoseAsset> PoseAsset::Load(const std::string& path)
{
    try
    {
        if (path.empty() || std::filesystem::path(path).extension() != ".pose")
            return std::nullopt;
        const auto root = Engine::Serialization::AssetParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            !root["name"].IsString() || !root["joints"].IsArray())
            return std::nullopt;
        PoseAsset result;
        result.name = root["name"].AsString();
        for (size_t i = 0; i < root["joints"].ArraySize(); ++i)
        {
            const auto& value = root["joints"].ArrayAt(i);
            if (!value["path"].IsString() ||
                value["rotation"].ArraySize() != 4) return std::nullopt;
            Joint joint;
            joint.path = value["path"].AsString();
            if (!ReadVector(value["position"], joint.position) ||
                !ReadVector(value["scale"], joint.scale)) return std::nullopt;
            float* rotation[] = {&joint.rotation.w, &joint.rotation.x,
                &joint.rotation.y, &joint.rotation.z};
            for (size_t j = 0; j < 4; ++j)
            {
                if (!value["rotation"].ArrayAt(j).IsNumber()) return std::nullopt;
                *rotation[j] = value["rotation"].ArrayAt(j).AsFloat();
            }
            result.joints.push_back(std::move(joint));
        }
        if (!ValidPose(result)) return std::nullopt;
        for (auto& joint : result.joints)
            joint.rotation = glm::normalize(joint.rotation);
        return result;
    }
    catch (...) { return std::nullopt; }
}
}
