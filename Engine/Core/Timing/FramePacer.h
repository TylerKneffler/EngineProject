#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace Engine::Core
{
// Limits frame starts without changing simulation delta time. Zero disables pacing.
class FramePacer
{
public:
    void Wait(uint32_t maxFps)
    {
        const auto now = Clock::now();
        if (maxFps == 0 || maxFps != m_maxFps || !m_started)
        {
            m_maxFps = maxFps;
            m_lastFrame = now;
            m_started = maxFps != 0;
            return;
        }
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / maxFps));
        const auto nextFrame = m_lastFrame + period;
        if (now < nextFrame)
            std::this_thread::sleep_until(nextFrame);
        m_lastFrame = Clock::now();
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastFrame{};
    uint32_t m_maxFps = 0;
    bool m_started = false;
};
}
