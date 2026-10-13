#include "AnimationClip.h"
#include <algorithm>
#include <cmath>

namespace Engine::Model
{
namespace
{
using Engine::Serialization::JsonValue;

JsonValue FloatArray(const std::vector<float>& values)
{
    JsonValue result = JsonValue::MakeArray();
    for (float value : values) result.Push(JsonValue(value));
    return result;
}

std::vector<float> ReadFloats(const JsonValue& value)
{
    std::vector<float> result;
    result.reserve(value.ArraySize());
    for (size_t i = 0; i < value.ArraySize(); ++i)
        result.push_back(value.ArrayAt(i).AsFloat());
    return result;
}
}

Engine::Serialization::JsonValue AnimationClip::Serialize() const
{
    JsonValue value = JsonValue::MakeObject()
        .Set("name", JsonValue(clipName))
        .Set("duration", JsonValue(duration))
        .Set("looping", JsonValue(looping));
    JsonValue serializedChannels = JsonValue::MakeArray();
    for (const auto& channel : channels)
        serializedChannels.Push(JsonValue::MakeObject()
            .Set("node", JsonValue(static_cast<int>(channel.nodeIndex)))
            .Set("targetPath", JsonValue(channel.targetPath))
            .Set("targetMesh", JsonValue(channel.targetMesh))
            .Set("path", JsonValue(static_cast<int>(channel.path)))
            .Set("interpolation", JsonValue(static_cast<int>(channel.interpolation)))
            .Set("width", JsonValue(static_cast<int>(channel.valueWidth)))
            .Set("times", FloatArray(channel.times))
            .Set("values", FloatArray(channel.values)));
    value.Set("channels", std::move(serializedChannels));
    JsonValue visibilityKeys = JsonValue::MakeArray();
    for (const auto& key : visibility)
        visibilityKeys.Push(JsonValue::MakeObject()
            .Set("time", JsonValue(key.time))
            .Set("visible", JsonValue(key.visible)));
    value.Set("visibility", std::move(visibilityKeys));
    JsonValue serializedLimbs = JsonValue::MakeArray();
    for (const auto& limb : limbs)
    {
        JsonValue keys = JsonValue::MakeArray();
        for (const auto& key : limb.keys)
            keys.Push(JsonValue::MakeObject()
                .Set("time", JsonValue(key.time))
                .Set("target", FloatArray({key.target.x, key.target.y, key.target.z}))
                .Set("pole", FloatArray({key.pole.x, key.pole.y, key.pole.z}))
                .Set("rotation", FloatArray({key.rotation.x, key.rotation.y, key.rotation.z, key.rotation.w}))
                .Set("blend", JsonValue(key.blend))
                .Set("space", JsonValue(static_cast<int>(key.space)))
                .Set("spaceNode", JsonValue(key.spaceNode))
                .Set("propPath", JsonValue(key.propReference.objectPath))
                .Set("propName", JsonValue(key.propReference.objectName))
                .Set("propType", JsonValue(key.propReference.componentType))
                .Set("propIndex", JsonValue(key.propReference.componentIndex)));
        serializedLimbs.Push(JsonValue::MakeObject()
            .Set("upper", JsonValue(static_cast<int>(limb.upper)))
            .Set("lower", JsonValue(static_cast<int>(limb.lower)))
            .Set("tip", JsonValue(static_cast<int>(limb.tip)))
            .Set("keys", std::move(keys)));
    }
    return value.Set("limbs", std::move(serializedLimbs));
}

void AnimationClip::Deserialize(const JsonValue& value)
{
    clipName = value["name"].AsString();
    duration = value["duration"].AsFloat();
    looping = !value.Has("looping") || value["looping"].AsBool();
    channels.clear();
    visibility.clear();
    limbs.clear();
    const JsonValue& visibilityKeys = value["visibility"];
    for (size_t i = 0; i < visibilityKeys.ArraySize(); ++i)
    {
        const auto& key = visibilityKeys.ArrayAt(i);
        const float keyTime = key["time"].AsFloat();
        if (keyTime >= 0.f && std::isfinite(keyTime))
            visibility.push_back({ keyTime, key["visible"].AsBool() });
    }
    std::stable_sort(visibility.begin(), visibility.end(),
        [](const VisibilityKey& a, const VisibilityKey& b)
        { return a.time < b.time; });
    const JsonValue& list = value["channels"];
    channels.reserve(list.ArraySize());
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        const JsonValue& item = list.ArrayAt(i);
        AnimationChannel channel;
        channel.nodeIndex = static_cast<unsigned>(item["node"].AsInt());
        if (item.Has("targetPath"))
            channel.targetPath = item["targetPath"].AsString();
        if (item.Has("targetMesh"))
            channel.targetMesh = item["targetMesh"].AsString();
        channel.path = static_cast<AnimationChannel::Path>(item["path"].AsInt());
        channel.interpolation = static_cast<AnimationChannel::Interpolation>(
            item["interpolation"].AsInt());
        channel.valueWidth = static_cast<unsigned>(item["width"].AsInt());
        channel.times = ReadFloats(item["times"]);
        channel.values = ReadFloats(item["values"]);
        channels.push_back(std::move(channel));
    }
    const JsonValue& limbList = value["limbs"];
    for (size_t i = 0; i < limbList.ArraySize(); ++i)
    {
        const auto& item = limbList.ArrayAt(i);
        LimbTrack limb;
        limb.upper = static_cast<unsigned>(item["upper"].AsInt());
        limb.lower = static_cast<unsigned>(item["lower"].AsInt());
        limb.tip = static_cast<unsigned>(item["tip"].AsInt());
        const auto& keys = item["keys"];
        for (size_t j = 0; j < keys.ArraySize(); ++j)
        {
            const auto& entry = keys.ArrayAt(j);
            LimbKey key;
            key.time = entry["time"].AsFloat();
            auto target = ReadFloats(entry["target"]);
            auto pole = ReadFloats(entry["pole"]);
            auto rotation = ReadFloats(entry["rotation"]);
            if (target.size() != 3 || pole.size() != 3 || rotation.size() != 4) continue;
            key.target = {target[0], target[1], target[2]};
            key.pole = {pole[0], pole[1], pole[2]};
            key.rotation = glm::normalize(glm::quat(rotation[3], rotation[0], rotation[1], rotation[2]));
            key.blend = std::clamp(entry["blend"].AsFloat(), 0.f, 1.f);
            if (entry.Has("space"))
            {
                const int space = entry["space"].AsInt();
                if (space >= 0 && space <= static_cast<int>(LimbKey::Space::Prop))
                    key.space = static_cast<LimbKey::Space>(space);
            }
            if (entry.Has("spaceNode")) key.spaceNode = entry["spaceNode"].AsInt();
            if (entry.Has("propType"))
            {
                key.propReference.objectPath = entry["propPath"].AsString();
                key.propReference.objectName = entry["propName"].AsString();
                key.propReference.componentType = entry["propType"].AsString();
                key.propReference.componentIndex = entry["propIndex"].AsInt();
            }
            limb.keys.push_back(key);
        }
        std::sort(limb.keys.begin(), limb.keys.end(),
            [](const LimbKey& a, const LimbKey& b) { return a.time < b.time; });
        if (!limb.keys.empty()) limbs.push_back(std::move(limb));
    }
}

AnimationClip::LimbKey AnimationClip::SampleLimb(const LimbTrack& track, float time)
{
    if (track.keys.empty()) return {};
    const auto next = std::upper_bound(track.keys.begin(), track.keys.end(), time,
        [](float t, const LimbKey& key) { return t < key.time; });
    if (next == track.keys.begin()) return *next;
    if (next == track.keys.end()) return track.keys.back();
    const LimbKey& a = *(next - 1);
    const LimbKey& b = *next;
    if (a.space != b.space || a.spaceNode != b.spaceNode ||
        a.propReference.objectPath != b.propReference.objectPath ||
        a.propReference.objectName != b.propReference.objectName)
    {
        LimbKey result = a;
        result.time = time;
        return result;
    }
    const float amount = std::clamp((time - a.time) / (b.time - a.time), 0.f, 1.f);
    LimbKey result;
    result.time = time;
    result.target = glm::mix(a.target, b.target, amount);
    result.pole = glm::mix(a.pole, b.pole, amount);
    result.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, amount));
    result.blend = glm::mix(a.blend, b.blend, amount);
    result.space = a.space;
    result.spaceNode = a.spaceNode;
    result.propReference = a.propReference;
    return result;
}
}
