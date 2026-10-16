#include "PerformanceRecording.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Engine::Editor
{
class PerformanceRecording::OutputTap final : public std::streambuf
{
public:
    OutputTap(PerformanceRecording& recording, std::streambuf* original,
        const char* source)
        : m_recording(recording), m_original(original), m_source(source) {}

protected:
    std::streamsize xsputn(const char* text, std::streamsize count) override
    {
        const auto written = m_original->sputn(text, count);
        for (std::streamsize index = 0; index < written; ++index)
        {
            if (text[index] == '\n')
            {
                m_recording.Event(m_source, "output", m_line);
                m_line.clear();
            }
            else m_line += text[index];
        }
        return written;
    }

    int overflow(int character) override
    {
        if (character == traits_type::eof()) return traits_type::not_eof(character);
        const char value = static_cast<char>(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }

    int sync() override
    {
        if (!m_line.empty())
        {
            m_recording.Event(m_source, "output", m_line);
            m_line.clear();
        }
        return m_original->pubsync();
    }

private:
    PerformanceRecording& m_recording;
    std::streambuf* m_original;
    const char* m_source;
    std::string m_line;
};

PerformanceRecording::PerformanceRecording() = default;

PerformanceRecording::~PerformanceRecording()
{
    Stop();
}

bool PerformanceRecording::Start(const std::filesystem::path& path)
{
    std::lock_guard lock(m_mutex);
    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open()) return false;
    m_originalStdout = std::cout.rdbuf();
    m_originalStderr = std::cerr.rdbuf();
    m_stdoutTap = std::make_unique<OutputTap>(*this, m_originalStdout, "stdout");
    m_stderrTap = std::make_unique<OutputTap>(*this, m_originalStderr, "stderr");
    std::cout.rdbuf(m_stdoutTap.get());
    std::cerr.rdbuf(m_stderrTap.get());
    return true;
}

void PerformanceRecording::Stop()
{
    if (m_stdoutTap)
    {
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(m_originalStdout);
        std::cerr.rdbuf(m_originalStderr);
        m_stdoutTap.reset();
        m_stderrTap.reset();
    }
    std::lock_guard lock(m_mutex);
    if (m_file.is_open()) m_file.close();
}

bool PerformanceRecording::IsRecording() const
{
    std::lock_guard lock(m_mutex);
    return m_file.is_open();
}

std::string PerformanceRecording::Escape(const std::string& value)
{
    std::ostringstream out;
    for (unsigned char character : value)
    {
        switch (character)
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (character < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(character) << std::dec;
            else out << character;
        }
    }
    return out.str();
}

std::string PerformanceRecording::Timestamp() const
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return out.str();
}

void PerformanceRecording::Event(const char* source, const char* level,
    const std::string& message)
{
    std::lock_guard lock(m_mutex);
    if (!m_file.is_open()) return;
    m_file << "{\"type\":\"output\",\"time_utc\":\"" << Timestamp()
        << "\",\"source\":\"" << Escape(source) << "\",\"level\":\""
        << Escape(level) << "\",\"message\":\"" << Escape(message)
        << "\"}\n";
}

void PerformanceRecording::Sample(const std::string& jsonFields)
{
    std::lock_guard lock(m_mutex);
    if (!m_file.is_open()) return;
    m_file << "{\"type\":\"sample\",\"time_utc\":\"" << Timestamp()
        << "\"," << jsonFields << "}\n";
}

void PerformanceRecording::Frame(float reportedFrameRate, double elapsedMs)
{
    std::lock_guard lock(m_mutex);
    if (!m_file.is_open()) return;
    m_file << "{\"type\":\"frame\",\"time_utc\":\"" << Timestamp()
        << "\",\"fps\":" << std::fixed << std::setprecision(3)
        << (elapsedMs > 0.0 ? 1000.0 / elapsedMs : 0.0)
        << ",\"frame_ms\":" << elapsedMs
        << ",\"ui_fps\":" << reportedFrameRate << "}\n";
}
}
