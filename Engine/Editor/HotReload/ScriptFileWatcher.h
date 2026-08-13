#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>

namespace Engine::Editor
{
class ScriptFileWatcher
{
public:
    void Initialize(std::string directory);
    bool Poll();
    bool HasFiles() const { return !m_files.empty(); }

private:
    using Stamp = std::filesystem::file_time_type;
    std::unordered_map<std::string, Stamp> Scan() const;
    std::filesystem::path m_directory;
    std::unordered_map<std::string, Stamp> m_files;
};
}
