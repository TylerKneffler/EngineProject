#pragma once

#include <string>

// A reusable 2D animation clip. The .spriteanim file selects an animation
// from a .spritesheet and supplies playback settings. It is intentionally
// separate from future skeletal/3D animation assets.
namespace Engine::Rendering
{
class SpriteAnimationAsset
{
public:
    std::string spriteSheetFile;
    std::string animation;
    float speed = 1.f;
    bool loop = true;
    bool autoplay = true;

    bool Load(const std::string& path);
    bool Save() const;
    bool SaveAs(const std::string& path);
    std::string ResolveSpriteSheet() const;
    const std::string& GetPath() const { return m_path; }

private:
    std::string m_path;
};
}

