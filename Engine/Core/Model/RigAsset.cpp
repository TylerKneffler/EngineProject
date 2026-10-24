#include "RigAsset.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
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
        result.bindNodes = AnimationClipAsset::CaptureBaseSkeleton(*model);
        if (result.bindNodes.size() != model->GetNodeCount())
            result.bindNodes.clear();
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

RigAsset RigAsset::ForModel(Engine::Components::Model& model)
{
    RigAsset result;
    Engine::Components::Skeleton* selected = nullptr;
    std::optional<RigAsset> selectedAsset;
    size_t selectedRoles = 0, selectedJoints = 0;
    if (model.Owner && model.Owner->GetScene())
        for (const auto& object : model.Owner->GetScene()->GetObjects())
            if (object) for (auto* component : object->Components)
                if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component);
                    skeleton && skeleton->ResolveModel() == &model)
                {
                    auto stored = Load(skeleton->rigPath,skeleton->skinIndex);
                    size_t roles = 0;
                    if (stored) for (int node : stored->humanoid.bones)
                        if (node>=0 && std::find(skeleton->jointNodes.begin(),skeleton->jointNodes.end(),unsigned(node))
                                != skeleton->jointNodes.end()) ++roles;
                    if (!selected || roles>selectedRoles ||
                        (roles==selectedRoles && skeleton->jointNodes.size()>selectedJoints))
                    {
                        selected=skeleton; selectedAsset=std::move(stored);
                        selectedRoles=roles; selectedJoints=skeleton->jointNodes.size();
                    }
                }
    if (selected)
    {
        result=Capture(*model.Owner->GetScene(),*selected);
        if (selectedAsset)
        {
            result.humanoid=selectedAsset->humanoid;
            result.prefabPath=selectedAsset->prefabPath;
            if (!selectedAsset->bindNodes.empty()) result.bindNodes=std::move(selectedAsset->bindNodes);
        }
    }
    if (result.bindNodes.empty()) result.bindNodes=AnimationClipAsset::CaptureBaseSkeleton(model);
    return result;
}

Engine::Serialization::JsonValue RigAsset::Serialize() const
{
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
    auto nodes = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& node : bindNodes)
    {
        const auto vector = [](const glm::vec3& value)
        {
            auto result = Engine::Serialization::JsonValue::MakeArray();
            for (int axis = 0; axis < 3; ++axis)
                result.Push(Engine::Serialization::JsonValue(value[axis]));
            return result;
        };
        nodes.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("path", Engine::Serialization::JsonValue(node.path))
            .Set("parent", Engine::Serialization::JsonValue(node.parent))
            .Set("position", vector(node.position))
            .Set("rotation", vector(node.rotation))
            .Set("scale", vector(node.scale)));
    }
    root.Set("bindNodes", std::move(nodes));
    auto meshes = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& name : meshNames)
        meshes.Push(Engine::Serialization::JsonValue(name));
    root.Set("meshes", std::move(meshes));
    auto animations = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& animation : animationPaths)
        animations.Push(Engine::Serialization::JsonValue(animation));
    root.Set("animations", std::move(animations));
    auto controlsValue = Engine::Serialization::JsonValue::MakeObject()
        .Set("root", Engine::Serialization::JsonValue(controls.root))
        .Set("pelvis", Engine::Serialization::JsonValue(controls.pelvis));
    auto limbsValue = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& limb : controls.limbs)
        limbsValue.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("upper", Engine::Serialization::JsonValue(limb.upper))
            .Set("lower", Engine::Serialization::JsonValue(limb.lower))
            .Set("tip", Engine::Serialization::JsonValue(limb.tip)));
    root.Set("controls", controlsValue.Set("limbs", std::move(limbsValue)));
    root.Set("humanoid", humanoid.Serialize());
    const auto vectorValue = [](const glm::vec3& value)
    {
        auto array = Engine::Serialization::JsonValue::MakeArray();
        for (int i = 0; i < 3; ++i)
            array.Push(Engine::Serialization::JsonValue(value[i]));
        return array;
    };
    auto posesValue = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& pose : poses)
    {
        auto jointsValue = Engine::Serialization::JsonValue::MakeArray();
        for (const auto& joint : pose.joints)
        {
            auto rotation = Engine::Serialization::JsonValue::MakeArray();
            for (float value : {joint.rotation.w, joint.rotation.x,
                    joint.rotation.y, joint.rotation.z})
                rotation.Push(Engine::Serialization::JsonValue(value));
            jointsValue.Push(Engine::Serialization::JsonValue::MakeObject()
                .Set("nodeIndex", Engine::Serialization::JsonValue(
                    static_cast<int>(joint.nodeIndex)))
                .Set("position", vectorValue(joint.position))
                .Set("rotation", std::move(rotation))
                .Set("scale", vectorValue(joint.scale)));
        }
        posesValue.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("name", Engine::Serialization::JsonValue(pose.name))
            .Set("joints", std::move(jointsValue)));
    }
    root.Set("poses", std::move(posesValue));
    auto pairsValue = Engine::Serialization::JsonValue::MakeArray();
    for (const auto& pair : symmetryPairs)
        pairsValue.Push(Engine::Serialization::JsonValue::MakeObject()
            .Set("left", Engine::Serialization::JsonValue(static_cast<int>(pair.left)))
            .Set("right", Engine::Serialization::JsonValue(static_cast<int>(pair.right))));
    root.Set("symmetryPairs", std::move(pairsValue));
    return root;
}

namespace
{
bool WriteRigAsset(const std::string& path,
    const Engine::Serialization::JsonValue& root)
{
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
        const std::string bytes = Engine::Serialization::AssetWriteBinary(root);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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
    if (!replaced)
        replaced = MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, target, error);
    replaced = !error;
#endif
    if (!replaced) std::filesystem::remove(temporary, error);
    return replaced;
}
}

bool RigAsset::Save(const std::string& path) const
{
    if (path.empty() || joints.empty()) return false;
    auto root = Serialize();
    try
    {
        const auto existing = Engine::Serialization::AssetParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (existing["version"].AsInt() == 2 &&
            existing["skins"].IsArray())
        {
            auto bundle = existing;
            bool found = false;
            for (size_t index = 0; index < bundle["skins"].ArraySize(); ++index)
                if (bundle["skins"].ArrayAt(index)["skinIndex"].AsInt() ==
                    static_cast<int>(skinIndex))
                {
                    bundle["skins"].ArrayAt(index) = std::move(root);
                    found = true;
                    break;
                }
            return found && WriteRigAsset(path, bundle);
        }
    }
    catch (...) { /* A new path is a standalone rig. */ }
    return WriteRigAsset(path, root);
}

bool RigAsset::SaveBundle(const std::string& path,
    const std::vector<RigAsset>& skins)
{
    if (path.empty() || skins.empty()) return false;
    auto entries = Engine::Serialization::JsonValue::MakeArray();
    for (size_t index = 0; index < skins.size(); ++index)
    {
        if (skins[index].joints.empty() || skins[index].skinIndex != index)
            return false;
        entries.Push(skins[index].Serialize());
    }
    return WriteRigAsset(path, Engine::Serialization::JsonValue::MakeObject()
        .Set("version", Engine::Serialization::JsonValue(2))
        .Set("skins", std::move(entries)));
}

std::optional<RigAsset> RigAsset::Load(const std::string& path,
    std::optional<unsigned> skinIndex)
{
    try
    {
        auto root = Engine::Serialization::AssetParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (root["version"].AsInt() == 2 && root["skins"].IsArray())
        {
            const auto& entries = root["skins"];
            if (entries.ArraySize() == 0) return std::nullopt;
            size_t selected = 0;
            if (skinIndex)
            {
                selected = entries.ArraySize();
                for (size_t index = 0; index < entries.ArraySize(); ++index)
                    if (entries.ArrayAt(index)["skinIndex"].AsInt() ==
                        static_cast<int>(*skinIndex))
                    { selected = index; break; }
                if (selected == entries.ArraySize()) return std::nullopt;
            }
            root = entries.ArrayAt(selected);
        }
        if (!root.IsObject() || root["version"].AsInt() != 1 ||
            !root["joints"].IsArray()) return std::nullopt;
        if (skinIndex && root["skinIndex"].AsInt() !=
            static_cast<int>(*skinIndex)) return std::nullopt;
        RigAsset result;
        result.skinIndex = static_cast<unsigned>(root["skinIndex"].AsInt());
        result.rigIndex = static_cast<size_t>(root["rigIndex"].AsInt());
        result.prefabPath = root["prefabPath"].AsString();
        for (size_t i = 0; i < root["bindNodes"].ArraySize(); ++i)
        {
            const auto& value = root["bindNodes"].ArrayAt(i);
            if (!value["path"].IsString()) return std::nullopt;
            AnimationClipAsset::BaseNode node;
            node.path = value["path"].AsString();
            node.parent = value["parent"].AsString();
            for (const auto* field : { "position", "rotation", "scale" })
                if (value[field].ArraySize() != 3) return std::nullopt;
            for (int axis = 0; axis < 3; ++axis)
            {
                node.position[axis] = value["position"].ArrayAt(axis).AsFloat();
                node.rotation[axis] = value["rotation"].ArrayAt(axis).AsFloat();
                node.scale[axis] = value["scale"].ArrayAt(axis).AsFloat();
            }
            result.bindNodes.push_back(std::move(node));
        }
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
        for (size_t i = 0; i < root["animations"].ArraySize(); ++i)
            result.animationPaths.push_back(
                root["animations"].ArrayAt(i).AsString());
        const auto& controls = root["controls"];
        result.humanoid = HumanoidRig::Mapping::Deserialize(root["humanoid"]);
        if (controls.IsObject())
        {
            result.controls.root = controls.Has("root")
                ? controls["root"].AsInt() : -1;
            result.controls.pelvis = controls.Has("pelvis")
                ? controls["pelvis"].AsInt() : -1;
            for (size_t i = 0; i < result.controls.limbs.size() &&
                i < controls["limbs"].ArraySize(); ++i)
            {
                const auto& limb = controls["limbs"].ArrayAt(i);
                result.controls.limbs[i] = {
                    limb.Has("upper") ? limb["upper"].AsInt() : -1,
                    limb.Has("lower") ? limb["lower"].AsInt() : -1,
                    limb.Has("tip") ? limb["tip"].AsInt() : -1};
            }
        }
        const auto readVector = [](const auto& value, glm::vec3& output)
        {
            if (value.ArraySize() != 3) return false;
            for (int i = 0; i < 3; ++i)
            {
                if (!value.ArrayAt(i).IsNumber()) return false;
                output[i] = value.ArrayAt(i).AsFloat();
            }
            return true;
        };
        for (size_t i = 0; i < root["poses"].ArraySize(); ++i)
        {
            const auto& source = root["poses"].ArrayAt(i);
            if (!source["name"].IsString() || !source["joints"].IsArray())
                return std::nullopt;
            Pose pose;
            pose.name = source["name"].AsString();
            for (size_t j = 0; j < source["joints"].ArraySize(); ++j)
            {
                const auto& value = source["joints"].ArrayAt(j);
                if (!value["nodeIndex"].IsNumber() ||
                    value["rotation"].ArraySize() != 4)
                    return std::nullopt;
                PoseJoint joint;
                joint.nodeIndex = static_cast<unsigned>(value["nodeIndex"].AsInt());
                if (!readVector(value["position"], joint.position) ||
                    !readVector(value["scale"], joint.scale)) return std::nullopt;
                float* rotation[] = {&joint.rotation.w, &joint.rotation.x,
                    &joint.rotation.y, &joint.rotation.z};
                for (size_t k = 0; k < 4; ++k)
                {
                    if (!value["rotation"].ArrayAt(k).IsNumber()) return std::nullopt;
                    *rotation[k] = value["rotation"].ArrayAt(k).AsFloat();
                }
                pose.joints.push_back(joint);
            }
            result.poses.push_back(std::move(pose));
        }
        for (size_t i = 0; i < root["symmetryPairs"].ArraySize(); ++i)
        {
            const auto& value = root["symmetryPairs"].ArrayAt(i);
            if (!value["left"].IsNumber() || !value["right"].IsNumber())
                return std::nullopt;
            result.symmetryPairs.push_back({
                static_cast<unsigned>(value["left"].AsInt()),
                static_cast<unsigned>(value["right"].AsInt())});
        }
        return result;
    }
    catch (...) { return std::nullopt; }
}

bool RigAsset::ApplySkeleton(Engine::Core::Object& owner,
    const std::string& assetPath, std::string& error) const
{
    error.clear();
    if (!owner.GetScene() || joints.empty() || bindNodes.empty())
    { error = "Skeleton has no standalone bind hierarchy. Split the source model again."; return false; }
    std::unordered_map<std::string, size_t> byPath;
    for (size_t i = 0; i < bindNodes.size(); ++i)
    {
        const auto& node = bindNodes[i];
        const auto slash = node.path.find_last_of('/');
        const std::string parent = slash == std::string::npos ? "" : node.path.substr(0, slash);
        if (node.path.empty() || node.parent != parent ||
            !byPath.emplace(node.path, i).second)
        { error = "Skeleton has an invalid or ambiguous node hierarchy."; return false; }
    }
    for (const auto& node : bindNodes)
        if (!node.parent.empty() && !byPath.count(node.parent))
        { error = "Skeleton is missing a parent node."; return false; }
    for (const auto& joint : joints)
        if (joint.nodeIndex >= bindNodes.size())
        { error = "Skeleton contains an invalid joint binding."; return false; }

    auto* model = owner.GetComponent<Engine::Components::Model>();
    if (!model) model = owner.AddComponent<Engine::Components::Model>();
    std::unordered_map<std::string, Engine::Core::Object*> objects;
    objects.emplace(".", &owner);
    const auto resolve = [&](auto&& self, size_t index) -> Engine::Core::Object*
    {
        const auto& node = bindNodes[index];
        if (const auto found = objects.find(node.path); found != objects.end())
            return found->second;
        auto* parent = node.parent.empty() ? &owner : self(self, byPath.at(node.parent));
        const std::string name = node.path.substr(node.path.find_last_of('/') ==
            std::string::npos ? 0 : node.path.find_last_of('/') + 1);
        Engine::Core::Object* object = nullptr;
        for (auto* child : parent->Children)
            if (child && child->name == name) { object = child; break; }
        if (!object)
        {
            object = owner.GetScene()->AddObject(name);
            object->Parent = parent;
            parent->Children.push_back(object);
            object->transform.position = node.position;
            object->transform.rotation = node.rotation;
            object->transform.scale = node.scale;
        }
        objects.emplace(node.path, object);
        return object;
    };
    std::vector<unsigned> mapped;
    for (size_t i = 0; i < bindNodes.size(); ++i)
    {
        auto* object = resolve(resolve, i);
        const auto& existing = model->ResolveNodes();
        const auto found = std::find(existing.begin(), existing.end(), object);
        unsigned index = found == existing.end() ? static_cast<unsigned>(model->GetNodeCount())
            : static_cast<unsigned>(found - existing.begin());
        if (found == existing.end()) model->BindNode(index, object);
        mapped.push_back(index);
    }
    auto* skeleton = owner.GetComponent<Engine::Components::Skeleton>();
    if (!skeleton) skeleton = owner.AddComponent<Engine::Components::Skeleton>();
    skeleton->rigPath = assetPath;
    skeleton->skinIndex = skinIndex;
    skeleton->jointNodes.clear();
    skeleton->rootBoneNodes.clear();
    skeleton->inverseBindMatrices.clear();
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const auto& joint = joints[i];
        const unsigned index = mapped[joint.nodeIndex];
        skeleton->jointNodes.push_back(index);
        skeleton->rootBoneNodes.push_back(index);
        skeleton->inverseBindMatrices.push_back(joint.inverseBind);
        auto* object = model->ResolveNode(index);
        auto* bone = object->GetComponent<Engine::Components::AnimationBone>();
        if (!bone) bone = object->AddComponent<Engine::Components::AnimationBone>();
        bone->nodeIndex = index;
        bone->skinIndex = skinIndex;
        bone->paletteIndex = static_cast<int>(i);
    }
    skeleton->MarkConfigurationDirty();
    return true;
}

std::vector<RigAsset> RigAsset::LoadAll(const std::string& path)
{
    std::vector<RigAsset> result;
    try
    {
        const auto root = Engine::Serialization::AssetParseFile(
            Engine::Components::Mesh::ResolveFilePath(path));
        if (root["version"].AsInt() == 2 && root["skins"].IsArray())
        {
            for (size_t index = 0; index < root["skins"].ArraySize(); ++index)
            {
                const auto skin = Load(path, static_cast<unsigned>(index));
                if (!skin) return {};
                result.push_back(*skin);
            }
            return result;
        }
    }
    catch (...) { return {}; }
    if (auto skin = Load(path)) result.push_back(*skin);
    return result;
}
}
