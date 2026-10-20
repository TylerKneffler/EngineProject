#pragma once

#include "AnimationClipAsset.h"
#include "RigAsset.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Object.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Model::AnimationRigMapping
{
struct Result
{
    AnimationClip clip;
    size_t unmatchedChannels = 0;
    size_t inferredChannels = 0;
    std::vector<std::string> unmatchedPaths;
};

inline std::string Leaf(const std::string& path)
{
    return path.substr(path.find_last_of('/') == std::string::npos
        ? 0 : path.find_last_of('/') + 1);
}

inline std::string Parent(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline std::string NormalizedName(std::string name)
{
    if (const size_t helper = name.find("$AssimpFbx$");
        helper != std::string::npos)
        name.resize(helper);
    std::string result;
    for (unsigned char ch : name)
        if (std::isalnum(ch))
            result += static_cast<char>(std::tolower(ch));
    if (result.rfind("mixamorig", 0) == 0)
        result.erase(0, 9);
    return result;
}

inline std::u32string Utf8Name(const std::string& name)
{
    std::u32string result;
    for (size_t index = 0; index < name.size();)
    {
        const unsigned char first = static_cast<unsigned char>(name[index]);
        const size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 :
            first < 0xf0 ? 3 : 4;
        if (index + width > name.size()) return {};
        char32_t value = first & (width == 1 ? 0x7f : width == 2 ? 0x1f :
            width == 3 ? 0x0f : 0x07);
        for (size_t byte = 1; byte < width; ++byte)
        {
            const unsigned char next = static_cast<unsigned char>(name[index + byte]);
            if ((next & 0xc0) != 0x80) return {};
            value = (value << 6) | (next & 0x3f);
        }
        result += value;
        index += width;
    }
    return result;
}

inline std::string MmdRole(const std::string& name)
{
    std::u32string value = Utf8Name(name);
    std::string side;
    if (!value.empty() && value.front() == U'\u5de6')
    { side = "l"; value.erase(0, 1); }
    else if (!value.empty() && value.front() == U'\u53f3')
    { side = "r"; value.erase(0, 1); }
    if (value == U"\u4e0b\u534a\u8eab") return "pelvis";
    if (value == U"\u4e0a\u534a\u8eab") return "spine1";
    if (value == U"\u4e0a\u534a\u8eab2") return "spine2";
    if (value == U"\u9996") return "neck";
    if (value == U"\u982d") return "head";
    if (side.empty()) return {};
    if (value == U"\u80a9") return side + "clavicle";
    if (value == U"\u8155") return side + "upperarm";
    if (value == U"\u3072\u3058") return side + "lowerarm";
    if (value == U"\u624b\u9996") return side + "hand";
    if (value == U"\u8db3") return side + "thigh";
    if (value == U"\u3072\u3056") return side + "calf";
    if (value == U"\u8db3\u9996") return side + "foot";
    if (value == U"\u3064\u307e\u5148") return side + "toe";
    if (value == U"\u89aa\u6307\uff10") return side + "thumb1";
    if (value == U"\u89aa\u6307\uff11") return side + "thumb2";
    if (value == U"\u89aa\u6307\uff12") return side + "thumb3";
    if (value == U"\u85ac\u6307\uff13l" && side == "l")
        value = U"\u85ac\u6307\uff13";
    for (const auto& finger : {
            std::pair<std::u32string, const char*>{ U"\u4eba\u6307", "index" },
            { U"\u4e2d\u6307", "middle" },
            { U"\u85ac\u6307", "ring" },
            { U"\u5c0f\u6307", "pinky" } })
        if (value.rfind(finger.first, 0) == 0 &&
            value.size() == finger.first.size() + 1)
        {
            const char32_t digit = value.back();
            if (digit >= U'\uff11' && digit <= U'\uff13')
                return side + finger.second +
                    std::to_string(static_cast<int>(digit - U'\uff10'));
        }
    return {};
}

// Standard Mixamo and UE humanoid joints. A role is intentionally absent for
// unfamiliar names: topology alone must not attach motion to an arbitrary bone.
inline std::string Role(const std::string& name)
{
    if (const std::string translated = MmdRole(name); !translated.empty())
        return translated;
    std::string value = NormalizedName(name);
    const bool numberedSpine = name.find("spine_") != std::string::npos;
    std::string side;
    if (value.rfind("left", 0) == 0)
    { side = "l"; value.erase(0, 4); }
    else if (value.rfind("right", 0) == 0)
    { side = "r"; value.erase(0, 5); }
    else if (value.size() > 1 && (value.back() == 'l' || value.back() == 'r'))
    {
        if (name.find('_') != std::string::npos)
        { side = value.back(); value.pop_back(); }
    }
    if (value == "hips" || value == "pelvis") value = "pelvis";
    else if (value == "spine") value = "spine1";
    else if (value == "spine1" || value == "spine01") value = numberedSpine ? "spine1" : "spine2";
    else if (value == "spine2" || value == "spine02") value = numberedSpine ? "spine2" : "spine3";
    else if (value == "spine03") value = "spine3";
    else if (value == "neck01") value = "neck";
    else if (value == "shoulder" || value == "clavicle") value = "clavicle";
    else if (value == "arm" || value == "upperarm") value = "upperarm";
    else if (value == "forearm" || value == "lowerarm") value = "lowerarm";
    else if (value == "upleg" || value == "thigh") value = "thigh";
    else if (value == "leg" || value == "calf") value = "calf";
    else if (value == "toebase" || value == "ball") value = "toe";
    else if (value != "hand" && value.rfind("hand", 0) == 0) value.erase(0, 4);
    for (const std::string finger : { "thumb", "index", "middle", "ring", "pinky" })
        if (value.rfind(finger, 0) == 0)
        {
            const int digit = value.size() > finger.size()
                ? std::atoi(value.c_str() + finger.size()) : 0;
            if (digit > 0) value = finger + std::to_string(digit);
            break;
        }
    if (value == "pelvis" || value == "neck" || value == "head" ||
        value == "spine1" || value == "spine2" || value == "spine3" ||
        value == "clavicle" || value == "upperarm" || value == "lowerarm" ||
        value == "hand" || value == "thigh" || value == "calf" ||
        value == "foot" || value == "toe" ||
        value.rfind("thumb", 0) == 0 || value.rfind("index", 0) == 0 ||
        value.rfind("middle", 0) == 0 || value.rfind("ring", 0) == 0 ||
        value.rfind("pinky", 0) == 0)
        return side + value;
    return {};
}

inline std::string AncestorRole(std::string path)
{
    while (!(path = Parent(path)).empty())
        if (const std::string role = Role(Leaf(path)); !role.empty())
            return role;
    return {};
}

inline std::string NodePath(const Engine::Components::Model& model,
    unsigned index)
{
    const auto* object = model.ResolveNode(index);
    if (!object || !model.Owner) return {};
    if (object == model.Owner) return ".";
    std::vector<std::string> names;
    for (auto* current = object; current && current != model.Owner;
        current = current->Parent)
        names.push_back(current->name);
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    { if (!path.empty()) path += '/'; path += *it; }
    return path;
}

inline Result Map(const AnimationClipAsset& source,
    const Engine::Components::Model& model, const RigAsset& rig)
{
    struct Target { unsigned index; std::string path, role, parentRole; bool joint; };
    std::vector<Target> targets;
    std::unordered_set<unsigned> joints;
    for (const auto& joint : rig.joints) joints.insert(joint.nodeIndex);
    for (unsigned index = 0; index < model.GetNodeCount(); ++index)
    {
        const std::string path = NodePath(model, index);
        if (!path.empty()) targets.push_back({ index, path, Role(Leaf(path)),
            AncestorRole(path), joints.count(index) != 0 });
    }
    Result result;
    result.clip = source.clip;
    result.clip.channels.clear();
    std::unordered_map<unsigned, std::string> owner;
    for (const auto& channel : source.clip.channels)
    {
        const std::string& path = channel.targetPath;
        const Target* match = nullptr;
        if (path.empty())
            for (const auto& target : targets)
                if (target.index == channel.nodeIndex)
                { match = &target; break; }
        for (const auto& target : targets)
            if (!match && target.path == path)
            { match = &target; break; }
        if (!match && !path.empty())
        {
            const std::string role = Role(Leaf(path));
            const std::string name = NormalizedName(Leaf(path));
            const std::string parentRole = AncestorRole(path);
            std::vector<const Target*> candidates;
            for (const auto& target : targets)
                if (target.joint && ((!role.empty() && target.role == role) ||
                    (role.empty() && !name.empty() &&
                        NormalizedName(Leaf(target.path)) == name)))
                    candidates.push_back(&target);
            if (candidates.size() > 1 && !parentRole.empty())
            {
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                    [&](const Target* candidate)
                    { return candidate->parentRole != parentRole; }), candidates.end());
            }
            if (candidates.size() == 1) match = candidates.front();
        }
        if (!match || (owner.count(match->index) && owner[match->index] != path))
        {
            ++result.unmatchedChannels;
            if (std::find(result.unmatchedPaths.begin(), result.unmatchedPaths.end(),
                    path) == result.unmatchedPaths.end())
                result.unmatchedPaths.push_back(path);
            auto unresolved = channel;
            unresolved.nodeIndex = std::numeric_limits<unsigned>::max();
            result.clip.channels.push_back(std::move(unresolved));
            continue;
        }
        owner[match->index] = path;
        auto mapped = channel;
        mapped.nodeIndex = match->index;
        mapped.targetPath = match->path;
        result.clip.channels.push_back(std::move(mapped));
        if (match->path != path) ++result.inferredChannels;
    }
    return result;
}
}
