#pragma once

#include <filesystem>
#include <future>
#include <cstdint>
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

    void Open(const std::filesystem::path& assetsRoot, bool deferInitialScan = false);
    std::vector<Move> Detect();
    void RequestDetect();
    bool PollDetect(std::vector<Move>& moves);
    void Accept();
    void RecordKnownMove(const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

private:
    struct Entry { std::string path; bool directory = false; };
    using Entries = std::unordered_map<std::string, Entry>;
    struct ScanResult { bool success = false; Entries entries; };
    static bool Scan(const std::filesystem::path& root, Entries& entries);
    std::vector<Move> Analyze(Entries current);
    bool Save() const;
    std::filesystem::path m_root;
    Entries m_entries;
    Entries m_scanned;
    std::future<ScanResult> m_pending;
    bool m_scanRequested = false;
    uint64_t m_revision = 0;
    uint64_t m_pendingRevision = 0;
};
}
