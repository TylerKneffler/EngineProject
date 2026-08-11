#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace Editor::ViewTemplates
{
inline std::string LowerAssetExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

inline bool IsTextureAssetExtension(const std::string& extension)
{
    static constexpr const char* extensions[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".dds", ".tga", ".hdr",
        ".exr", ".ktx2"
    };
    return std::find_if(std::begin(extensions), std::end(extensions),
        [&extension](const char* candidate) { return extension == candidate; }) !=
        std::end(extensions);
}

inline bool IsAudioAssetExtension(const std::string& extension)
{
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}
}
