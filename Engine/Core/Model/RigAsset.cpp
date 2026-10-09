#include "RigAsset.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Model
{
RigAsset RigAsset::Capture(const Engine::Scene::Scene& scene,
    const Engine::Components::Skeleton& skeleton)
{
    RigAsset result;
    result.skinIndex = skeleton.skinIndex;
    const auto ordinal = [&]() -> size_t
    {
        size_t index = 0;
        for (const auto& object : scene.GetObjects())
            if (object)
                for (auto* component : object->Components)
                    if (auto* candidate = dynamic_cast<const
                            Engine::Components::Skeleton*>(component))
                    {
                        if (candidate == &skeleton) return index;
                        ++index;
                    }
        return index;
    };
    result.rigIndex = ordinal();
    const auto& nodes = skeleton.ResolveJoints();
    std::unordered_map<const Engine::Core::Object*, unsigned> indices;
    if (auto* model = skeleton.ResolveModel())
    {
        const auto& modelNodes = model->ResolveNodes();
        for (size_t i = 0; i < modelNodes.size(); ++i)
            if (modelNodes[i]) indices[modelNodes[i]] =
                static_cast<unsigned>(i);
    }
    for (size_t i = 0; i < skeleton.jointNodes.size(); ++i)
    {
        Joint joint;
        joint.nodeIndex = skeleton.jointNodes[i];
        if (i < nodes.size() && nodes[i])
        {
            joint.name = nodes[i]->name;
            for (auto* parent = nodes[i]->Parent; parent; parent = parent->Parent)
                if (const auto found = indices.find(parent);
                    found != indices.end())
                { joint.parentNodeIndex = static_cast<int>(found->second); break; }
        }
        if (i < skeleton.inverseBindMatrices.size())
            joint.inverseBind = skeleton.inverseBindMatrices[i];
        result.joints.push_back(std::move(joint));
    }
    for (const auto& object : scene.GetObjects())
        if (object)
            for (auto* component : object->Components)
                if (auto* skin = dynamic_cast<const Engine::Components::SkinnedMesh*>(component);
                    skin && skin->ResolveSkeleton() == &skeleton)
                    result.meshNames.push_back(object->name);
    return result;
}

bool RigAsset::Save(const std::string& path) const
{
    if (path.empty() || joints.empty()) return false;
    Engine::Serialization::JsonValue root =
        Engine::Serialization::JsonValue::MakeObject();
    root.Set("version", Engine::Serialization::JsonValue(1));
    root.Set("skinIndex", Engine::Serialization::JsonValue(
        static_cast<int>(skinIndex)));
    root.Set("rigIndex", Engine::Serialization::JsonValue(
        static_cast<int>(rigIndex)));
    root.Set("prefabPath", Engine::Serialization::JsonValue(prefabPath));
    auto serializedJoints = Engine::Serialization::JsonValue::MakeArray();
    for (const Joint& joint : joints)
    {
        auto matrix = Engine::Serialization::JsonValue::MakeArray();
        const float* values = &joint.inverseBind[0][0];
        for (size_t i = 0; i < 16; ++i)
            matrix.Push(Engine::Serialization::JsonValue(values[i]));
        serializedJoints.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("nodeIndex", Engine::Serialization::JsonValue(
                static_cast<int>(joint.nodeIndex)))
            .Set("name", Engine::Serialization::JsonValue(joint.name))
            .Set("parentNodeIndex", Engine::Serialization::JsonValue(
                joint.parentNodeIndex))
            .Set("inverseBind", std::move(matrix)));
    }
    root.Set("joints", std::move(serializedJoints));
    auto meshes = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& name : meshNames)
        meshes.Push(Engine::Serialization::JsonValue(name));
    root.Set("meshes", std::move(meshes));
    std::filesystem::path target(path);
    if (!target.parent_path().empty())
        target = std::filesystem::path(
            Engine::Components::Mesh::ResolveFilePath(
                target.parent_path().string())) / target.filename();
    const std::filesystem::path temporary = target.string() + ".tmp." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << Engine::Serialization::JsonWrite(root) << '\n';
        if (!output)
        { std::filesystem::remove(temporary, error); return false; }
    }
    bool replaced = false;
#ifdef _WIN32
    const std::wstring source = temporary.wstring();
    const std::wstring destination = target.wstring();
    replaced = std::filesystem::exists(target)
        ? ReplaceFileW(destination.c_str(), source.c_str(), nullptr,
            REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0
        : MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, target, error);
    replaced = !error;
#endif
    if (!replaced) std::filesystem::remove(temporary, error);
    return replaced;
}

std::optional<RigAsset> RigAsset::Load(const std::string& path)
{
    try
    {
        const auto root = Engine::Serialization::JsonParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            !root["joints"].IsArray()) return std::nullopt;
        RigAsset result;
        result.skinIndex = static_cast<unsigned>(root["skinIndex"].AsInt());
        result.rigIndex = static_cast<size_t>(root["rigIndex"].AsInt());
        result.prefabPath = root["prefabPath"].AsString();
        for (size_t i = 0; i < root["joints"].ArraySize(); ++i)
        {
            const auto& source = root["joints"].ArrayAt(i);
            if (!source.IsObject() || !source["nodeIndex"].IsNumber() ||
                !source["name"].IsString() ||
                source["inverseBind"].ArraySize() != 16)
                return std::nullopt;
            Joint joint;
            joint.nodeIndex = static_cast<unsigned>(source["nodeIndex"].AsInt());
            joint.name = source["name"].AsString();
            joint.parentNodeIndex = source["parentNodeIndex"].AsInt();
            float* values = &joint.inverseBind[0][0];
            for (size_t value = 0; value < 16; ++value)
                values[value] = source["inverseBind"].ArrayAt(value).AsFloat();
            result.joints.push_back(std::move(joint));
        }
        if (result.joints.empty()) return std::nullopt;
        for (size_t i = 0; i < root["meshes"].ArraySize(); ++i)
            result.meshNames.push_back(root["meshes"].ArrayAt(i).AsString());
        return result;
    }
    catch (...) { return std::nullopt; }
}
}
