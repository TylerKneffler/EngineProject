#include "PerformanceRecording.h"
#include "Diagnostics/PerformanceProvenance.h"
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
        std::lock_guard lock(m_mutex);
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
        std::lock_guard lock(m_mutex);
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
    std::mutex m_mutex;
};

PerformanceRecording::PerformanceRecording() = default;

PerformanceRecording::~PerformanceRecording()
{
    Stop();
}

bool PerformanceRecording::Start(const std::filesystem::path& path)
{
    if(IsRecording()) return false;
    Stop(); // Join a completed/error writer and restore any previous output taps.
    std::lock_guard lock(m_mutex);
    if (!m_writer.Start(path)) return false;
    m_frameIndex = 0;
    const auto time = std::chrono::system_clock::now();
    const auto provenance = PerformanceProvenanceFields();
    m_writer.Enqueue([time, provenance] {
        return "{\"type\":\"session\",\"time_utc\":\"" + Timestamp(time) + "\"," +
            provenance + "}";
    }, provenance.size() + 256);
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
    m_writer.Stop();
}

bool PerformanceRecording::IsRecording() const
{
    return m_writer.IsOpen();
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

std::string PerformanceRecording::Timestamp(std::chrono::system_clock::time_point now)
{
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
    const auto time=std::chrono::system_clock::now();
    const size_t bytes=message.size()+std::char_traits<char>::length(source)+std::char_traits<char>::length(level)+256;
    m_writer.Enqueue([source=std::string(source),level=std::string(level),message,time] {
        return "{\"type\":\"output\",\"time_utc\":\""+Timestamp(time)+"\",\"source\":\""+
            Escape(source)+"\",\"level\":\""+Escape(level)+"\",\"message\":\""+Escape(message)+"\"}";
    },bytes);
}

void PerformanceRecording::Sample(const std::string& jsonFields)
{
    Engine::Threading::CapturedText fields; fields<<jsonFields; Sample(std::move(fields));
}

void PerformanceRecording::Sample(Engine::Threading::CapturedText fields)
{
    std::lock_guard lock(m_mutex);
    const auto time=std::chrono::system_clock::now();
    const auto bytes=fields.OwnedBytes()+256;
    m_writer.Enqueue([fields=std::move(fields),time] {
        return "{\"type\":\"sample\",\"time_utc\":\""+Timestamp(time)+"\","+fields.Format()+"}";
    },bytes);
}

void PerformanceRecording::FrameDetails(const std::string& jsonFields)
{
    Engine::Threading::CapturedText fields; fields<<jsonFields; FrameDetails(std::move(fields));
}

void PerformanceRecording::FrameDetails(Engine::Threading::CapturedText fields)
{
    std::lock_guard lock(m_mutex);
    if(!m_writer.IsOpen()) return;
    const auto index=m_frameIndex++;
    const auto time=std::chrono::system_clock::now();
    const auto bytes=fields.OwnedBytes()+256;
    m_writer.Enqueue([fields=std::move(fields),index,time] {
        return "{\"type\":\"frame_details\",\"time_utc\":\""+Timestamp(time)+"\",\"frame\":"+
            std::to_string(index)+","+fields.Format()+"}";
    },bytes);
}

void PerformanceRecording::Frame(float reportedFrameRate, double elapsedMs,
    double panelDrawMs, double submittedFramesPerSecond)
{
    std::lock_guard lock(m_mutex);
    const auto time=std::chrono::system_clock::now();
    m_writer.Enqueue([reportedFrameRate,elapsedMs,panelDrawMs,submittedFramesPerSecond,time] {
        std::ostringstream output; output.imbue(std::locale::classic());
        output << "{\"type\":\"frame\",\"time_utc\":\"" << Timestamp(time)
            << "\",\"fps\":" << std::fixed << std::setprecision(3)
            << (elapsedMs>0 ? 1000./elapsedMs:0.) << ",\"frame_ms\":" << elapsedMs
            << ",\"ui_fps\":" << reportedFrameRate << ",\"fps_scope\":\"editor_update_interval\""
            << ",\"editor_updates_per_second\":" << (elapsedMs>0 ? 1000./elapsedMs:0.)
            << ",\"submitted_frames_per_second\":";
        if(submittedFramesPerSecond>=0) output << submittedFramesPerSecond;
        else output << "null";
        output << ",\"panel_draw_ms\":" << panelDrawMs << "}";
        return output.str();
    },256);
}
}
