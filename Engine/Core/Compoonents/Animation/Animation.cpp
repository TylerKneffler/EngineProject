#include "Animation.h"

namespace
{
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

Animation::Animation() { SetTypeName(COMPONENT_TYPE_NAME(Animation)); }

JsonValue Animation::Serialize() const
{
    JsonValue value = JsonValue::MakeObject().Set("type", JsonValue(GetTypeName()))
        .Set("name", JsonValue(clipName)).Set("duration", JsonValue(duration));
    JsonValue serializedChannels = JsonValue::MakeArray();
    for (const auto& channel : channels)
    {
        serializedChannels.Push(JsonValue::MakeObject()
            .Set("node", JsonValue(static_cast<int>(channel.nodeIndex)))
            .Set("path", JsonValue(static_cast<int>(channel.path)))
            .Set("interpolation", JsonValue(static_cast<int>(channel.interpolation)))
            .Set("width", JsonValue(static_cast<int>(channel.valueWidth)))
            .Set("times", FloatArray(channel.times))
            .Set("values", FloatArray(channel.values)));
    }
    return value.Set("channels", std::move(serializedChannels));
}

void Animation::Deserialize(const JsonValue& value)
{
    clipName = value["name"].AsString();
    duration = value["duration"].AsFloat();
    channels.clear();
    const JsonValue& list = value["channels"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        const JsonValue& item = list.ArrayAt(i);
        AnimationChannel channel;
        channel.nodeIndex = static_cast<unsigned>(item["node"].AsInt());
        channel.path = static_cast<AnimationChannel::Path>(item["path"].AsInt());
        channel.interpolation = static_cast<AnimationChannel::Interpolation>(item["interpolation"].AsInt());
        channel.valueWidth = static_cast<unsigned>(item["width"].AsInt());
        channel.times = ReadFloats(item["times"]);
        channel.values = ReadFloats(item["values"]);
        channels.push_back(std::move(channel));
    }
}
