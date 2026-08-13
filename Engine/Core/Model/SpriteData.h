#pragma once

#include <string>
#include <vector>

namespace Engine::Model
{
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
}

