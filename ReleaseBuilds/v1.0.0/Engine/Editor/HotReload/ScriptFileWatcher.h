#pragma once
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace Engine::Editor
{
class ScriptFileWatcher
{
public:
    ScriptFileWatcher();
    ~ScriptFileWatcher();
    ScriptFileWatcher(const ScriptFileWatcher&) = delete;
    ScriptFileWatcher& operator=(const ScriptFileWatcher&) = delete;

    void Initialize(std::string directory);
    bool Poll();
    bool HasFiles() const { return !m_files.empty(); }

private:
    using Stamp = std::filesystem::file_time_type;
    struct NotificationState;

    std::unordered_map<std::string, Stamp> Scan() const;
    bool PollFallback(std::chrono::steady_clock::time_point now);
    std::filesystem::path m_directory;
    std::unordered_map<std::string, Stamp> m_files;
    std::unique_ptr<NotificationState> m_notifications;
    std::chrono::steady_clock::time_point m_nextFallbackScan{};
    std::chrono::steady_clock::time_point m_lastRelevantChange{};
    bool m_debouncedChangePending = false;
};
}
