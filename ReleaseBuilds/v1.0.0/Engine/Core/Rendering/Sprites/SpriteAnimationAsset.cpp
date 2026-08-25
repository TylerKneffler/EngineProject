#include "SpriteAnimationAsset.h"
#include "Core/Serialization/Json.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Engine::Rendering
{
bool SpriteAnimationAsset::Load(const std::string& path)
{
    try
    {
        const Engine::Serialization::JsonValue root = Engine::Serialization::JsonParseFile(path);
        if (!root.IsObject())
            return false;
        spriteSheetFile = root.Has("spriteSheet") ? root["spriteSheet"].AsString()
            : (root.Has("spriteSheetFile") ? root["spriteSheetFile"].AsString() : std::string{});
        animation = root.Has("animation") ? root["animation"].AsString() : std::string{};
        speed = std::max(root.Has("speed") ? root["speed"].AsFloat() : 1.f, 0.f);
        loop = !root.Has("loop") || root["loop"].AsBool();
        autoplay = !root.Has("autoplay") || root["autoplay"].AsBool();
        m_path = std::filesystem::path(path).lexically_normal().generic_string();
        return !spriteSheetFile.empty();
    }
    catch (...)
    {
        return false;
    }
}

bool SpriteAnimationAsset::Save() const
{
    if (m_path.empty())
        return false;
    Engine::Serialization::JsonValue root = Engine::Serialization::JsonValue::MakeObject();
    root.Set("type", Engine::Serialization::JsonValue("sprite-animation"));
    root.Set("spriteSheet", Engine::Serialization::JsonValue(spriteSheetFile));
    root.Set("animation", Engine::Serialization::JsonValue(animation));
    root.Set("speed", Engine::Serialization::JsonValue(std::max(speed, 0.f)));
    root.Set("loop", Engine::Serialization::JsonValue(loop));
    root.Set("autoplay", Engine::Serialization::JsonValue(autoplay));
    std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << Engine::Serialization::JsonWrite(root) << '\n';
    return static_cast<bool>(output);
}

bool SpriteAnimationAsset::SaveAs(const std::string& path)
{
    m_path = std::filesystem::path(path).lexically_normal().generic_string();
    return Save();
}

std::string SpriteAnimationAsset::ResolveSpriteSheet() const
{
    if (spriteSheetFile.empty())
        return {};
    const std::filesystem::path requested(spriteSheetFile);
    if (requested.is_absolute() || std::filesystem::exists(requested))
        return requested.lexically_normal().generic_string();
    return (std::filesystem::path(m_path).parent_path() / requested)
        .lexically_normal().generic_string();
}
}
