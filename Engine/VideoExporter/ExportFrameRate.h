#pragma once

#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>

namespace Engine::Video
{
struct ExportFrameRate
{
    uint32_t numerator = 60u;
    uint32_t denominator = 1u;

    bool IsValid() const
    {
        return numerator >= denominator && denominator > 0u &&
            static_cast<uint64_t>(numerator) <=
                static_cast<uint64_t>(240u) * denominator;
    }

    double AsDouble() const
    {
        return static_cast<double>(numerator) / denominator;
    }

    std::string ToString() const
    {
        return denominator == 1u ? std::to_string(numerator)
            : std::to_string(numerator) + "/" + std::to_string(denominator);
    }

    uint64_t FramesForDuration(double seconds) const
    {
        if (seconds <= 0.0) return 0u;
        return static_cast<uint64_t>(std::ceil(
            static_cast<long double>(seconds) * numerator / denominator));
    }

    double Timestamp(uint64_t frame) const
    {
        return static_cast<double>(frame) * denominator / numerator;
    }

    uint64_t AudioFramesAt(uint64_t videoFrame, uint32_t sampleRate) const
    {
        return videoFrame * static_cast<uint64_t>(sampleRate) * denominator /
            numerator;
    }

    void Normalize()
    {
        const uint32_t divisor = std::gcd(numerator, denominator);
        if (divisor > 0u)
        {
            numerator /= divisor;
            denominator /= divisor;
        }
    }
};
}
