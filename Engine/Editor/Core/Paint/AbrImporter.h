#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Editor::Paint
{
struct AbrTip
{
    std::string name;
    uint32_t width = 0, height = 0;
    float spacing = .25f;
    std::vector<uint8_t> mask;
};

// Extracts sampled tips. Unsupported brush records are reported in notes.
bool ReadAbrTips(const std::vector<uint8_t>& bytes,
    std::vector<AbrTip>& tips, std::string& notes);
bool ImportAbrPack(const std::filesystem::path& source,
    const std::filesystem::path& assetsDirectory, std::string& report);
}
