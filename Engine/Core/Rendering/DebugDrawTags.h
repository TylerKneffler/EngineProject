#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

// Names for draws, for GPU hang reports (ENGINE_GPU_DRED=1).
//
// The renderer gives a draw a tag (a number) and a description; the backend
// records the number as a marker in the command list. When the device is
// removed, the report lists the markers nearest the operation that did not
// finish and looks their descriptions up here. The last few frames are kept,
// because the frame that hangs is one or two behind the one being recorded.
// Off (and free) unless ENGINE_GPU_DRED is set.
namespace Engine::Rendering
{
class DebugDrawTags
{
public:
    static bool Enabled()
    {
        static const bool enabled = []
        {
            const char* value = std::getenv("ENGINE_GPU_DRED");
            return value && value[0] != 0 && value[0] != '0';
        }();
        return enabled;
    }
    static DebugDrawTags& Get() { static DebugDrawTags tags; return tags; }

    // Starts the tags of a new frame (any number; the prepared frame serial).
    void BeginFrame(uint64_t frame)
    {
        std::lock_guard lock(m_mutex);
        if (m_frames[m_current].frame == frame) return;
        m_current = (m_current + 1) % m_frames.size();
        m_frames[m_current].frame = frame;
        m_frames[m_current].text.clear();
    }
    // A tag for the marker: frame in the high half, index in the low half.
    uint64_t Add(std::string description)
    {
        std::lock_guard lock(m_mutex);
        auto& frame = m_frames[m_current];
        frame.text.push_back(std::move(description));
        return (frame.frame << 32u) | uint64_t(frame.text.size() - 1u);
    }
    std::string Describe(uint64_t tag)
    {
        std::lock_guard lock(m_mutex);
        const uint64_t frameId = tag >> 32u, index = tag & 0xffffffffull;
        for (const auto& frame : m_frames)
            if ((frame.frame & 0xffffffffull) == frameId && index < frame.text.size())
                return frame.text[size_t(index)];
        return "(description no longer kept)";
    }

    // Every kept description, oldest frame first: "frame index: text" lines.
    // The n-th marker of a command list is the n-th description of its view.
    template<class Stream> void DumpAll(Stream& output)
    {
        std::lock_guard lock(m_mutex);
        for (size_t step = 1; step <= m_frames.size(); ++step)
        {
            const auto& frame = m_frames[(m_current + step) % m_frames.size()];
            if (frame.frame == UINT64_MAX) continue;
            for (size_t index = 0; index < frame.text.size(); ++index)
                output << '\n' << "TAG " << frame.frame << ' ' << index << ": " << frame.text[index];
        }
    }

private:
    struct Frame { uint64_t frame = UINT64_MAX; std::vector<std::string> text; };
    std::array<Frame, 6> m_frames;
    size_t m_current = 0;
    std::mutex m_mutex;
};
}
