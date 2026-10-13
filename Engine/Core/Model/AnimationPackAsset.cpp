#include "AnimationPackAsset.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/Serialization/Json.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_map>
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

bool Validate(const AnimationPackAsset& pack, std::string& error)
{
    if (pack.clips.empty() && pack.sourceTracks.empty())
    { error = "Animation pack contains no clips or source tracks"; return false; }
    std::unordered_set<std::string> names;
    for (const auto& clip : pack.clips)
        if (clip.clipName.empty() || !names.insert(clip.clipName).second)
        { error = "Animation pack has an empty or duplicate clip name"; return false; }
    if (!pack.activeClip.empty() && !names.count(pack.activeClip))
    { error = "Animation pack active clip is missing"; return false; }
    for (const auto& layer : pack.layers)
        if (!names.count(layer.clip))
        { error = "Animation pack layer clip is missing: " + layer.clip; return false; }
    if (!pack.nodeMaskPaths.empty() &&
        pack.nodeMaskPaths.size() != pack.layers.size())
    { error = "Animation pack layer masks are malformed"; return false; }
    std::unordered_set<std::string> kinds;
    for (const auto& track : pack.sourceTracks)
        if (track.kind.empty() || track.json.empty() ||
            !kinds.insert(track.kind).second)
        { error = "Animation pack has an invalid or duplicate source track"; return false; }
    return true;
}

std::unordered_map<std::string, unsigned> NodePaths(
    const Engine::Components::Model& model)
{
    std::unordered_map<std::string, unsigned> paths;
    const auto& nodes = model.ResolveNodes();
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        std::vector<std::string> names;
        auto* current = nodes[index];
        for (; current && current != model.Owner; current = current->Parent)
            names.push_back(current->name);
        if (current != model.Owner) continue;
        std::string path;
        for (auto it = names.rbegin(); it != names.rend(); ++it)
        {
            if (!path.empty()) path += '/';
            path += *it;
        }
        paths.emplace(path.empty() ? "." : path, static_cast<unsigned>(index));
    }
    return paths;
}

std::unordered_map<std::string, size_t> MorphMeshWidths(
    const Engine::Components::Model& model)
{
    std::unordered_map<std::string, size_t> widths;
    const auto visit = [&](auto&& self, const Engine::Core::Object* object,
        const std::string& path) -> void
    {
        if (!object) return;
        if (const auto* mesh = object->GetComponent<Engine::Components::Mesh>())
            widths.emplace(path, mesh->GetMorphTargets().size());
        for (const auto* child : object->Children)
            self(self, child, path.empty() ? child->name : path + "/" + child->name);
    };
    visit(visit, model.Owner, "");
    return widths;
}
}

AnimationPackAsset AnimationPackAsset::Capture(
    const Engine::Components::AnimationManager& manager)
{
    AnimationPackAsset pack;
    pack.activeClip = manager.clip;
    pack.playing = manager.playing;
    pack.looping = manager.looping;
    pack.speed = manager.speed;
    pack.time = manager.time;
    pack.layers = manager.layers;
    const Engine::Components::Model* model = nullptr;
    for (auto* object = manager.Owner; object && !model; object = object->Parent)
        model = object->GetComponent<Engine::Components::Model>();
    std::unordered_map<unsigned, std::string> pathsByIndex;
    if (model)
        for (const auto& [path, index] : NodePaths(*model))
            pathsByIndex.emplace(index, path);
    for (const auto& layer : manager.layers)
    {
        std::vector<std::string> paths;
        for (unsigned index : layer.nodeMask)
            if (const auto found = pathsByIndex.find(index);
                found != pathsByIndex.end()) paths.push_back(found->second);
        pack.nodeMaskPaths.push_back(std::move(paths));
    }
    for (const auto* clip : manager.GetAvailableClips())
        if (clip) pack.clips.push_back(*clip);
    return pack;
}

std::optional<AnimationPackAsset> AnimationPackAsset::Load(
    const std::string& path, std::string& error)
{
    try
    {
        const JsonValue root = Engine::Serialization::AssetParseFile(path);
        if (root["format"].AsString() != "engine-animation-pack" ||
            root["version"].AsInt() != 1 || !root["clips"].IsArray() ||
            !root["layers"].IsArray())
        { error = "Unsupported animation pack format"; return std::nullopt; }
        AnimationPackAsset pack;
        pack.activeClip = root["activeClip"].AsString();
        pack.playing = !root.Has("playing") || root["playing"].AsBool();
        pack.looping = !root.Has("looping") || root["looping"].AsBool();
        pack.speed = root.Has("speed") ? root["speed"].AsFloat() : 1.f;
        pack.time = root["time"].AsFloat();
        for (size_t i = 0; i < root["clips"].ArraySize(); ++i)
        {
            const auto& stored = root["clips"].ArrayAt(i);
            const JsonValue value = stored.IsString()
                ? Engine::Serialization::JsonParse(stored.AsString()) : stored;
            if (!value["name"].IsString() || !value["channels"].IsArray())
            { error = "Animation pack contains an invalid clip"; return std::nullopt; }
            AnimationClip clip;
            clip.Deserialize(value);
            pack.clips.push_back(std::move(clip));
        }
        for (size_t i = 0; i < root["layers"].ArraySize(); ++i)
        {
            const auto& value = root["layers"].ArrayAt(i);
            AnimationLayer layer;
            layer.clip = value["clip"].AsString();
            layer.weight = value.Has("weight") ? value["weight"].AsFloat() : 1.f;
            layer.speed = value.Has("speed") ? value["speed"].AsFloat() : 1.f;
            layer.time = value["time"].AsFloat();
            layer.enabled = !value.Has("enabled") || value["enabled"].AsBool();
            layer.looping = !value.Has("looping") || value["looping"].AsBool();
            layer.additive = value.Has("additive") && value["additive"].AsBool();
            for (size_t node = 0; node < value["nodeMask"].ArraySize(); ++node)
                layer.nodeMask.push_back(static_cast<unsigned>(
                    value["nodeMask"].ArrayAt(node).AsInt()));
            pack.layers.push_back(std::move(layer));
            std::vector<std::string> maskPaths;
            for (size_t node = 0; node < value["nodeMaskPaths"].ArraySize(); ++node)
                maskPaths.push_back(value["nodeMaskPaths"].ArrayAt(node).AsString());
            pack.nodeMaskPaths.push_back(std::move(maskPaths));
        }
        for (size_t i = 0; i < root["sourceTracks"].ArraySize(); ++i)
        {
            const auto& item = root["sourceTracks"].ArrayAt(i);
            pack.sourceTracks.push_back({ item["kind"].AsString(),
                item["json"].AsString() });
        }
        if (!Validate(pack, error)) return std::nullopt;
        return pack;
    }
    catch (const std::exception& exception)
    { error = exception.what(); return std::nullopt; }
}

bool AnimationPackAsset::Save(const std::string& path, std::string& error) const
{
    if (!Validate(*this, error)) return false;
    JsonValue root = JsonValue::MakeObject()
        .Set("format", JsonValue("engine-animation-pack"))
        .Set("version", JsonValue(1))
        .Set("activeClip", JsonValue(activeClip))
        .Set("playing", JsonValue(playing))
        .Set("looping", JsonValue(looping))
        .Set("speed", JsonValue(speed))
        .Set("time", JsonValue(time));
    JsonValue savedClips = JsonValue::MakeArray();
    // Keep only one clip's channel tree in memory at a time. Large baked
    // dances contain hundreds of thousands of numeric key values.
    for (const auto& clip : clips)
        savedClips.Push(JsonValue(
            Engine::Serialization::JsonWrite(clip.Serialize())));
    root.Set("clips", std::move(savedClips));
    JsonValue savedLayers = JsonValue::MakeArray();
    for (size_t index = 0; index < layers.size(); ++index)
    {
        const auto& layer = layers[index];
        JsonValue mask = JsonValue::MakeArray();
        for (unsigned node : layer.nodeMask)
            mask.Push(JsonValue(static_cast<int>(node)));
        JsonValue maskPaths = JsonValue::MakeArray();
        if (index < nodeMaskPaths.size())
            for (const auto& nodePath : nodeMaskPaths[index])
                maskPaths.Push(JsonValue(nodePath));
        savedLayers.Push(JsonValue::MakeObject()
            .Set("clip", JsonValue(layer.clip))
            .Set("weight", JsonValue(layer.weight))
            .Set("speed", JsonValue(layer.speed))
            .Set("time", JsonValue(layer.time))
            .Set("enabled", JsonValue(layer.enabled))
            .Set("looping", JsonValue(layer.looping))
            .Set("additive", JsonValue(layer.additive))
            .Set("nodeMask", std::move(mask))
            .Set("nodeMaskPaths", std::move(maskPaths)));
    }
    root.Set("layers", std::move(savedLayers));
    JsonValue savedTracks = JsonValue::MakeArray();
    for (const auto& track : sourceTracks)
        savedTracks.Push(JsonValue::MakeObject()
            .Set("kind", JsonValue(track.kind))
            .Set("json", JsonValue(track.json)));
    root.Set("sourceTracks", std::move(savedTracks));
    const std::filesystem::path target(path);
    std::error_code ioError;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ioError);
    if (ioError)
    { error = ioError.message(); return false; }
    const std::filesystem::path temporary = target.string() + ".tmp." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string bytes = Engine::Serialization::AssetWriteBinary(root);
        if (!file || !file.write(bytes.data(),
            static_cast<std::streamsize>(bytes.size())))
        { error = "Could not write animation pack"; return false; }
    }
    bool replaced = false;
#ifdef _WIN32
    replaced = std::filesystem::exists(target)
        ? ReplaceFileW(target.wstring().c_str(), temporary.wstring().c_str(),
            nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0
        : MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
            MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, target, ioError);
    replaced = !ioError;
#endif
    if (!replaced)
    {
        std::filesystem::remove(temporary, ioError);
        error = "Could not replace animation pack: " + path;
    }
    return replaced;
}

bool AnimationPackAsset::Apply(Engine::Components::AnimationManager& manager,
    std::string& error) const
{
    if (!Validate(*this, error)) return false;
    if (clips.empty())
    { error = "Pack has source tracks only; bind or extract a track before playback"; return false; }
    Engine::Components::Model* model = nullptr;
    for (auto* object = manager.Owner; object && !model; object = object->Parent)
        model = object->GetComponent<Engine::Components::Model>();
    if (!model)
    { error = "Animation pack needs a target model"; return false; }
    const auto paths = NodePaths(*model);
    const auto meshWidths = MorphMeshWidths(*model);
    auto remapped = clips;
    for (auto& clip : remapped)
        for (auto& channel : clip.channels)
        {
            if (channel.targetPath.empty())
            {
                if (channel.nodeIndex >= model->GetNodeCount())
                { error = "Animation pack contains an out-of-range node"; return false; }
                continue;
            }
            const auto found = paths.find(channel.targetPath);
            if (found == paths.end())
            { error = "Target model lacks animation node: " + channel.targetPath; return false; }
            channel.nodeIndex = found->second;
        }
    for (const auto& clip : remapped)
        for (const auto& channel : clip.channels)
            if (channel.path == AnimationChannel::Path::Weights &&
                !channel.targetMesh.empty())
            {
                const auto found = meshWidths.find(channel.targetMesh);
                if (found == meshWidths.end() || found->second != channel.valueWidth)
                { error = "Target model lacks matching facial mesh: " +
                    channel.targetMesh; return false; }
            }
    auto remappedLayers = layers;
    for (size_t index = 0; index < remappedLayers.size(); ++index)
    {
        auto& mask = remappedLayers[index].nodeMask;
        if (index < nodeMaskPaths.size() && !nodeMaskPaths[index].empty())
        {
            mask.clear();
            for (const auto& path : nodeMaskPaths[index])
            {
                const auto found = paths.find(path);
                if (found == paths.end())
                { error = "Target model lacks masked node: " + path; return false; }
                mask.push_back(found->second);
            }
        }
        else
            for (unsigned node : mask)
                if (node >= model->GetNodeCount())
                { error = "Animation pack contains an out-of-range mask node"; return false; }
    }
    manager.InstallEmbeddedClips(std::move(remapped));
    manager.clip = activeClip;
    manager.playing = playing;
    manager.looping = looping;
    manager.speed = speed;
    manager.time = time;
    manager.layers = std::move(remappedLayers);
    manager.Start();
    return true;
}

bool AnimationPackAsset::ExtractSourceTrack(const std::string& kind,
    const std::string& path, std::string& error) const
{
    const auto found = std::find_if(sourceTracks.begin(), sourceTracks.end(),
        [&](const auto& track) { return track.kind == kind; });
    if (found == sourceTracks.end())
    { error = "Animation pack has no " + kind + " source track"; return false; }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !(file << found->json << '\n'))
    { error = "Could not export source track: " + path; return false; }
    return true;
}
}
