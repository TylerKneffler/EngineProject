#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <streambuf>
#include <string>

namespace Engine::Editor
{
// Shared by the Console and Performance panels so console messages are retained
// even when the Console window is hidden or cleared during a recording.
class PerformanceRecording
{
public:
    PerformanceRecording();
    ~PerformanceRecording();
    bool Start(const std::filesystem::path& path);
    void Stop();
    bool IsRecording() const;
    void Event(const char* source, const char* level, const std::string& message);
    void Frame(float reportedFrameRate, double elapsedMs);
    void Sample(const std::string& jsonFields);

private:
    class OutputTap;
    static std::string Escape(const std::string& value);
    std::string Timestamp() const;
    mutable std::mutex m_mutex;
    std::ofstream m_file;
    std::unique_ptr<OutputTap> m_stdoutTap;
    std::unique_ptr<OutputTap> m_stderrTap;
    std::streambuf* m_originalStdout = nullptr;
    std::streambuf* m_originalStderr = nullptr;
};
}
