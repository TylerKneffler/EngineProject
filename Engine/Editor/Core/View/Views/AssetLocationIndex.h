#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Editor
{
// Project-local record of filesystem identities and their last accepted paths.
// File identities survive renames and moves within a Windows volume.
class AssetLocationIndex
{
public:
    struct Move
    {
        std::string oldPath;
        std::string newPath;
        bool directory = false;
    };

    void Open(const std::filesystem::path& assetsRoot);
    std::vector<Move> Detect();
    void Accept();
    void RecordKnownMove(const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

private:
    struct Entry { std::string path; bool directory = false; };
    bool Scan(std::unordered_map<std::string, Entry>& entries) const;
    bool Save() const;
    std::filesystem::path m_root;
    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_map<std::string, Entry> m_scanned;
};
}
