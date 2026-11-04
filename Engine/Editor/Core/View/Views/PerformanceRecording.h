#pragma once
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <memory>
#include <streambuf>
#include <string>
#include "Core/Threading/AsyncTextWriter.h"
#include "Core/Threading/CapturedText.h"

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
    void Frame(float reportedFrameRate, double elapsedMs,
        double panelDrawMs = 0.0, double submittedFramesPerSecond = -1.0);
    void Sample(const std::string& jsonFields);
    void Sample(Engine::Threading::CapturedText fields);
    void FrameDetails(const std::string& jsonFields);
    void FrameDetails(Engine::Threading::CapturedText fields);
    Engine::Threading::AsyncTextWriter::Statistics GetWriterStatistics() const { return m_writer.ReadStatistics(); }

private:
    class OutputTap;
    static std::string Escape(const std::string& value);
    static std::string Timestamp(std::chrono::system_clock::time_point time);
    mutable std::mutex m_mutex;
    Engine::Threading::AsyncTextWriter m_writer;
    uint64_t m_frameIndex = 0;
    std::unique_ptr<OutputTap> m_stdoutTap;
    std::unique_ptr<OutputTap> m_stderrTap;
    std::streambuf* m_originalStdout = nullptr;
    std::streambuf* m_originalStderr = nullptr;
};
}
