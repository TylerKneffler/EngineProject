#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>

namespace Engine::Rendering
{
struct TextureStreamingSettings
{
    // Startup opt-in: preserve full-quality reference captures when zero.
    uint64_t budgetBytes = 0;
    uint64_t uploadBytesPerFrame = 128ull * 1024 * 1024;
    uint32_t holdFrames = 30;
    uint32_t prefetchMips = 1;
    uint32_t maxDecodes = 2;
};

inline uint32_t TextureDemandMip(uint32_t width, uint32_t height,
    uint32_t levels, float pixelsPerRepeat, uint32_t prefetch = 1)
{
    if (!levels) return 0;
    if (std::isnan(pixelsPerRepeat) || std::isinf(pixelsPerRepeat)) return 0;
    if (!(pixelsPerRepeat > 0.f)) return levels - 1;
    const float lod = std::floor(std::log2(float(std::max(width,height)) / pixelsPerRepeat));
    return uint32_t(std::clamp(lod - float(prefetch), 0.f, float(levels - 1)));
}

// Sliding minimum: high detail expires even while a receding object stays visible.
class TextureMipHistory
{
public:
    uint32_t Update(uint64_t frame, uint32_t mip, uint32_t hold)
    {
        while (!m_recent.empty() && m_recent.back().second >= mip) m_recent.pop_back();
        m_recent.emplace_back(frame,mip);
        while (frame - m_recent.front().first > hold) m_recent.pop_front();
        return m_recent.front().second;
    }
    void Reset() { m_recent.clear(); }
private:
    std::deque<std::pair<uint64_t,uint32_t>> m_recent;
};
}
