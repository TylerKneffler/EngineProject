#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct SpriteSheetFrame
{
    std::string image;
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
    float duration = 0.1f;
};

struct SpriteSheetAnimation
{
    std::string name;
    bool loop = true;
    std::vector<SpriteSheetFrame> frames;
};

// Runtime datatype for .spritesheet assets. Files may contain JSON or XML;
// image references are resolved relative to the spritesheet file.
class SpriteSheetAsset
{
public:
    bool Load(const std::string& path);
    const SpriteSheetAnimation* FindAnimation(const std::string& name) const;
    const SpriteSheetAnimation* GetDefaultAnimation() const;
    const std::string& GetPath() const { return m_path; }
    const std::vector<SpriteSheetAnimation>& GetAnimations() const { return m_animations; }

private:
    std::string ResolveImage(const std::string& image) const;
    bool LoadJson(const std::string& source);
    bool LoadXml(const std::string& source);

    std::string m_path;
    std::vector<SpriteSheetAnimation> m_animations;
};
