#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace Engine::Editor::Paint
{
inline bool TipUsesLuminance(const std::vector<uint8_t>& pixels,
    uint32_t width, uint32_t height)
{
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    if (!width || !height || pixels.size() < bytes) return false;
    for (size_t alpha = 3; alpha < bytes; alpha += 4)
        if (pixels[alpha] != 255) return false;
    return true;
}

inline float SampleTipMask(const std::vector<uint8_t>& pixels,
    uint32_t width, uint32_t height, float localX, float localY,
    bool useLuminance)
{
    if (!width || !height || pixels.size() <
            static_cast<size_t>(width) * height * 4 ||
        localX < -1.f || localX > 1.f ||
        localY < -1.f || localY > 1.f) return 0.f;
    const uint32_t x = std::min(width - 1,
        static_cast<uint32_t>(std::clamp(localX * .5f + .5f, 0.f, 1.f) * width));
    const uint32_t y = std::min(height - 1,
        static_cast<uint32_t>(std::clamp(localY * .5f + .5f, 0.f, 1.f) * height));
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
    return useLuminance ?
        1.f - (pixels[offset] + pixels[offset + 1] +
            pixels[offset + 2]) / 765.f : pixels[offset + 3] / 255.f;
}
}
