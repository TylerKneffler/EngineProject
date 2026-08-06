#include "SpriteSheetAsset.h"
#include "Core/Serialization/Json.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
float Number(const JsonValue& object, const char* name, float fallback)
{
    return object.Has(name) ? object[name].AsFloat() : fallback;
}

SpriteSheetFrame ReadJsonFrame(const JsonValue& value,
    const std::string& defaultImage)
{
    SpriteSheetFrame frame;
    frame.image = value.Has("image") ? value["image"].AsString() : defaultImage;
    frame.x = Number(value, "x", 0.f);
    frame.y = Number(value, "y", 0.f);
    frame.width = Number(value, "width", Number(value, "w", 0.f));
    frame.height = Number(value, "height", Number(value, "h", 0.f));
    frame.duration = std::max(Number(value, "duration", 0.1f), 0.001f);
    return frame;
}
}

bool SpriteSheetAsset::Load(const std::string& path)
{
    m_path = std::filesystem::path(path).lexically_normal().generic_string();
    m_animations.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string source = contents.str();
    const auto first = std::find_if_not(source.begin(), source.end(),
        [](unsigned char character) { return std::isspace(character) != 0; });
    if (first == source.end())
        return false;
    try
    {
        const bool loaded = *first == '{' || *first == '['
            ? LoadJson(source) : LoadXml(source);
        if (!loaded)
            m_animations.clear();
        for (SpriteSheetAnimation& animation : m_animations)
            for (SpriteSheetFrame& frame : animation.frames)
                frame.image = ResolveImage(frame.image);
        return loaded && !m_animations.empty();
    }
    catch (...)
    {
        m_animations.clear();
        return false;
    }
}

std::string SpriteSheetAsset::ResolveImage(const std::string& image) const
{
    if (image.empty())
        return {};
    const std::filesystem::path requested(image);
    if (requested.is_absolute() || std::filesystem::exists(requested))
        return requested.lexically_normal().generic_string();
    return (std::filesystem::path(m_path).parent_path() / requested)
        .lexically_normal().generic_string();
}

bool SpriteSheetAsset::LoadJson(const std::string& source)
{
    const JsonValue root = JsonParse(source);
    const std::string defaultImage = root.Has("image")
        ? root["image"].AsString()
        : (root.Has("texture") ? root["texture"].AsString() : std::string{});
    const JsonValue& animations = root["animations"];
    if (animations.IsArray())
    {
        for (size_t index = 0; index < animations.ArraySize(); ++index)
        {
            const JsonValue& value = animations.ArrayAt(index);
            SpriteSheetAnimation animation;
            animation.name = value.Has("name") ? value["name"].AsString()
                : "Animation " + std::to_string(index);
            animation.loop = !value.Has("loop") || value["loop"].AsBool();
            const JsonValue& frames = value["frames"];
            for (size_t frame = 0; frame < frames.ArraySize(); ++frame)
                animation.frames.push_back(ReadJsonFrame(frames.ArrayAt(frame), defaultImage));
            if (!animation.frames.empty())
                m_animations.push_back(std::move(animation));
        }
    }
    else if (animations.IsObject())
    {
        for (size_t index = 0; index < animations.ObjectSize(); ++index)
        {
            const JsonValue& value = animations.ObjectValue(index);
            SpriteSheetAnimation animation;
            animation.name = animations.ObjectKey(index);
            animation.loop = !value.Has("loop") || value["loop"].AsBool();
            const JsonValue& frames = value["frames"];
            for (size_t frame = 0; frame < frames.ArraySize(); ++frame)
                animation.frames.push_back(ReadJsonFrame(frames.ArrayAt(frame), defaultImage));
            if (!animation.frames.empty())
                m_animations.push_back(std::move(animation));
        }
    }
    if (m_animations.empty() && root.Has("frames"))
    {
        SpriteSheetAnimation animation;
        animation.name = "Default";
        const JsonValue& frames = root["frames"];
        for (size_t frame = 0; frame < frames.ArraySize(); ++frame)
            animation.frames.push_back(ReadJsonFrame(frames.ArrayAt(frame), defaultImage));
        if (!animation.frames.empty())
            m_animations.push_back(std::move(animation));
    }
    return !m_animations.empty();
}

bool SpriteSheetAsset::LoadXml(const std::string& source)
{
    pugi::xml_document document;
    if (!document.load_string(source.c_str()))
        return false;
    const pugi::xml_node root = document.document_element();
    const std::string defaultImage = root.attribute("image")
        ? root.attribute("image").value() : root.attribute("texture").value();
    for (pugi::xml_node node : root.children("Animation"))
    {
        SpriteSheetAnimation animation;
        animation.name = node.attribute("name").as_string("Default");
        animation.loop = node.attribute("loop").as_bool(true);
        for (pugi::xml_node frameNode : node.children("Frame"))
        {
            SpriteSheetFrame frame;
            frame.image = frameNode.attribute("image").as_string(defaultImage.c_str());
            frame.x = frameNode.attribute("x").as_float();
            frame.y = frameNode.attribute("y").as_float();
            frame.width = frameNode.attribute("width").as_float(
                frameNode.attribute("w").as_float());
            frame.height = frameNode.attribute("height").as_float(
                frameNode.attribute("h").as_float());
            frame.duration = std::max(frameNode.attribute("duration").as_float(0.1f), 0.001f);
            animation.frames.push_back(std::move(frame));
        }
        if (!animation.frames.empty())
            m_animations.push_back(std::move(animation));
    }
    return !m_animations.empty();
}

const SpriteSheetAnimation* SpriteSheetAsset::FindAnimation(
    const std::string& name) const
{
    const auto found = std::find_if(m_animations.begin(), m_animations.end(),
        [&name](const SpriteSheetAnimation& animation)
        {
            return animation.name == name;
        });
    return found == m_animations.end() ? nullptr : &*found;
}

const SpriteSheetAnimation* SpriteSheetAsset::GetDefaultAnimation() const
{
    return m_animations.empty() ? nullptr : &m_animations.front();
}
