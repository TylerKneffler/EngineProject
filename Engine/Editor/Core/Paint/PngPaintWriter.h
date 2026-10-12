#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Editor::Paint
{
bool SavePng(const std::filesystem::path& path, uint32_t width,
    uint32_t height, const std::vector<uint8_t>& rgba, std::string& error);
}
