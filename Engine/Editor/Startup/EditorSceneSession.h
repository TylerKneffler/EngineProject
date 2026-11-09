#pragma once

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

namespace Engine::Editor
{
    inline std::filesystem::path SceneSessionFile(const std::string& projectFile)
    {
        const auto workspace = projectFile.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(projectFile).parent_path();
        const auto name = projectFile.empty() ? std::string("sandbox")
            : std::filesystem::path(projectFile).filename().string();
        return workspace / ".local" / (name + ".last-scene");
    }

    inline std::string RestoreLastScene(const std::string& projectFile,
        const std::string& fallback)
    {
        try
        {
            const auto file = SceneSessionFile(projectFile);
            std::ifstream input(file);
            std::string scene;
            if (input >> std::quoted(scene); input && !scene.empty())
            {
                auto path = std::filesystem::path(scene);
                if (path.is_relative()) path = file.parent_path().parent_path() / path;
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) return path.string();
            }
        }
        catch (const std::filesystem::filesystem_error&) {}
        return fallback;
    }

    inline void RememberLastScene(const std::string& projectFile,
        const std::string& scene)
    {
        if (scene.empty()) return;
        try
        {
            const auto file = SceneSessionFile(projectFile);
            const auto path = std::filesystem::absolute(scene);
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            if (error) return;
            std::ofstream output(file, std::ios::trunc);
            if (output) output << std::quoted(path.generic_string());
        }
        catch (const std::filesystem::filesystem_error&) {}
    }
}
