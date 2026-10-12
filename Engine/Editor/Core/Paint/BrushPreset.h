#pragma once

#include <filesystem>
#include <string>

namespace Engine::Editor::Paint
{
// A project asset. Stroke color/weight, target, and bone selection live in the
// tool session, so changing a preset never changes the content being painted.
struct BrushPreset
{
    std::string name = "Round";
    std::string tipPath; // Assets-relative image path; empty means analytic tip.
    int shape = 0;       // circle, square
    int falloff = 2;     // hard, linear, smooth
    float size = 40.f;   // viewport pixels
    float hardness = .5f;
    float strength = .35f;
    float opacity = 1.f;
    float flow = 1.f;
    float spacing = .08f; // fraction of diameter
    float rotation = 0.f; // degrees
    float scatter = 0.f;  // fraction of radius
    float pressureSize = 0.f;
    float pressureStrength = 0.f;
};

bool SaveBrushPreset(const std::filesystem::path& path,
    const BrushPreset& preset, std::string& error);
bool LoadBrushPreset(const std::filesystem::path& path,
    BrushPreset& preset, std::string& error);
}
