#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

namespace Engine::Editor::Paint
{
// RGBA8 canvas operations. Inputs are image pixel coordinates, so a repeated
// sequence of samples produces byte-identical pixels independent of zoom.
inline bool Stamp(std::vector<uint8_t>& pixels, uint32_t width,
    uint32_t height, float centerX, float centerY, float radius,
    float hardness, float opacity, const uint8_t color[4], bool erase,
    int falloffMode = 2, const std::vector<uint8_t>* tip = nullptr,
    uint32_t tipWidth = 0, uint32_t tipHeight = 0,
    bool tipUseLuminance = false, bool alphaLock = false,
    bool flipTipX = false, bool flipTipY = false)
{
    if (!width || !height || pixels.size() !=
            static_cast<size_t>(width) * height * 4 ||
        !std::isfinite(centerX) || !std::isfinite(centerY) ||
        !std::isfinite(radius) || radius <= 0.f) return false;
    const int left = std::max(0, static_cast<int>(std::floor(centerX - radius)));
    const int top = std::max(0, static_cast<int>(std::floor(centerY - radius)));
    const int right = std::min(static_cast<int>(width) - 1,
        static_cast<int>(std::ceil(centerX + radius)));
    const int bottom = std::min(static_cast<int>(height) - 1,
        static_cast<int>(std::ceil(centerY + radius)));
    hardness = std::clamp(hardness, 0.f, 1.f);
    opacity = std::clamp(opacity, 0.f, 1.f);
    bool changed = false;
    for (int y = top; y <= bottom; ++y)
    for (int x = left; x <= right; ++x)
    {
        const float distance = std::hypot(x + .5f - centerX,
            y + .5f - centerY) / radius;
        if (distance > 1.f) continue;
        float falloff = distance <= hardness ? 1.f :
            hardness >= 1.f ? 0.f :
            (1.f - distance) / (1.f - hardness);
        if (falloffMode == 0) falloff = distance <= hardness ? 1.f : 0.f;
        else if (falloffMode == 2)
            falloff = falloff * falloff * (3.f - 2.f * falloff);
        float amount = std::clamp(opacity * falloff, 0.f, 1.f);
        if (tip && tipWidth && tipHeight &&
            tip->size() == static_cast<size_t>(tipWidth) * tipHeight * 4)
        {
            const float u = (x + .5f - centerX) / (radius * 2.f) + .5f;
            const float v = (y + .5f - centerY) / (radius * 2.f) + .5f;
            uint32_t tx = std::min(tipWidth - 1,
                static_cast<uint32_t>(std::clamp(u, 0.f, 1.f) * tipWidth));
            uint32_t ty = std::min(tipHeight - 1,
                static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * tipHeight));
            if (flipTipX) tx = tipWidth - 1 - tx;
            if (flipTipY) ty = tipHeight - 1 - ty;
            const size_t tipOffset =
                (static_cast<size_t>(ty) * tipWidth + tx) * 4;
            amount *= tipUseLuminance ?
                1.f - ((*tip)[tipOffset] + (*tip)[tipOffset + 1] +
                    (*tip)[tipOffset + 2]) / 765.f :
                (*tip)[tipOffset + 3] / 255.f;
        }
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        const uint8_t before[4]{ pixels[offset], pixels[offset + 1],
            pixels[offset + 2], pixels[offset + 3] };
        if (erase && alphaLock) continue;
        if (erase)
            pixels[offset + 3] = static_cast<uint8_t>(std::lround(
                before[3] * (1.f - amount)));
        else
        {
            const float sourceAlpha = amount * color[3] / 255.f;
            const float destinationAlpha = before[3] / 255.f;
            const float outputAlpha = sourceAlpha +
                destinationAlpha * (1.f - sourceAlpha);
            if (outputAlpha > 0.f)
                for (int channel = 0; channel < 3; ++channel)
                    pixels[offset + channel] = static_cast<uint8_t>(
                        std::lround((color[channel] * sourceAlpha +
                            before[channel] * destinationAlpha *
                                (1.f - sourceAlpha)) / outputAlpha));
            pixels[offset + 3] = static_cast<uint8_t>(
                std::lround(outputAlpha * 255.f));
            if (alphaLock) pixels[offset + 3] = before[3];
        }
        for (int channel = 0; channel < 4; ++channel)
            changed |= pixels[offset + channel] != before[channel];
    }
    return changed;
}

inline bool Fill(std::vector<uint8_t>& pixels, uint32_t width,
    uint32_t height, uint32_t seedX, uint32_t seedY, float tolerance,
    const uint8_t color[4])
{
    if (!width || !height || seedX >= width || seedY >= height ||
        pixels.size() != static_cast<size_t>(width) * height * 4 ||
        !std::isfinite(tolerance)) return false;
    tolerance = std::clamp(tolerance, 0.f, 1.f);
    const size_t seed = static_cast<size_t>(seedY) * width + seedX;
    const uint8_t initial[4]{ pixels[seed * 4], pixels[seed * 4 + 1],
        pixels[seed * 4 + 2], pixels[seed * 4 + 3] };
    const int threshold = static_cast<int>(std::lround(tolerance * 255.f));
    std::vector<bool> visited(static_cast<size_t>(width) * height, false);
    std::queue<size_t> pending;
    pending.push(seed); visited[seed] = true;
    bool changed = false;
    while (!pending.empty())
    {
        const size_t pixel = pending.front(); pending.pop();
        const size_t offset = pixel * 4;
        bool matching = true;
        for (int channel = 0; channel < 4; ++channel)
            matching &= std::abs(int(pixels[offset + channel]) -
                int(initial[channel])) <= threshold;
        if (!matching) continue;
        for (int channel = 0; channel < 4; ++channel)
        {
            changed |= pixels[offset + channel] != color[channel];
            pixels[offset + channel] = color[channel];
        }
        const size_t x = pixel % width, y = pixel / width;
        const auto visit = [&](size_t neighbor)
        {
            if (!visited[neighbor])
            { visited[neighbor] = true; pending.push(neighbor); }
        };
        if (x > 0) visit(pixel - 1);
        if (x + 1 < width) visit(pixel + 1);
        if (y > 0) visit(pixel - width);
        if (y + 1 < height) visit(pixel + width);
    }
    return changed;
}

inline void AverageColor(const std::vector<uint8_t>& pixels,
    uint32_t width, uint32_t height, float centerX, float centerY,
    float radius, uint8_t result[4])
{
    uint64_t totals[4]{};
    uint64_t count = 0;
    for (int y = std::max(0, static_cast<int>(centerY - radius));
        y < std::min(static_cast<int>(height),
            static_cast<int>(centerY + radius) + 1); ++y)
    for (int x = std::max(0, static_cast<int>(centerX - radius));
        x < std::min(static_cast<int>(width),
            static_cast<int>(centerX + radius) + 1); ++x)
    {
        if (std::hypot(x + .5f - centerX, y + .5f - centerY) > radius)
            continue;
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
            totals[channel] += pixels[offset + channel];
        ++count;
    }
    if (!count) return;
    for (int channel = 0; channel < 4; ++channel)
        result[channel] = static_cast<uint8_t>(totals[channel] / count);
}

inline bool BlurStamp(std::vector<uint8_t>& pixels, uint32_t width,
    uint32_t height, float centerX, float centerY, float radius,
    float opacity, float hardness = .5f, int falloffMode = 2,
    bool alphaLock = false)
{
    if (!width || !height || pixels.size() !=
        static_cast<size_t>(width) * height * 4 || radius <= 0.f)
        return false;
    const auto source = pixels;
    bool changed = false;
    for (int y = std::max(0, static_cast<int>(centerY - radius));
        y < std::min(static_cast<int>(height),
            static_cast<int>(centerY + radius) + 1); ++y)
    for (int x = std::max(0, static_cast<int>(centerX - radius));
        x < std::min(static_cast<int>(width),
            static_cast<int>(centerX + radius) + 1); ++x)
    {
        const float distance = std::hypot(x + .5f - centerX,
            y + .5f - centerY) / radius;
        if (distance > 1.f) continue;
        hardness = std::clamp(hardness, 0.f, 1.f);
        float falloff = distance <= hardness ? 1.f :
            hardness >= 1.f ? 0.f :
            (1.f - distance) / (1.f - hardness);
        if (falloffMode == 0) falloff = distance <= hardness ? 1.f : 0.f;
        else if (falloffMode == 2)
            falloff = falloff * falloff * (3.f - 2.f * falloff);
        const float amount = std::clamp(opacity * falloff, 0.f, 1.f);
        uint32_t totals[4]{};
        int count = 0;
        for (int neighborY = std::max(0, y - 1);
            neighborY <= std::min(static_cast<int>(height) - 1, y + 1);
            ++neighborY)
        for (int neighborX = std::max(0, x - 1);
            neighborX <= std::min(static_cast<int>(width) - 1, x + 1);
            ++neighborX)
        {
            const size_t offset =
                (static_cast<size_t>(neighborY) * width + neighborX) * 4;
            for (int channel = 0; channel < 4; ++channel)
                totals[channel] += source[offset + channel];
            ++count;
        }
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
        {
            if (alphaLock && channel == 3) continue;
            const uint8_t result = static_cast<uint8_t>(std::lround(
                source[offset + channel] * (1.f - amount) +
                (totals[channel] / static_cast<float>(count)) * amount));
            changed |= pixels[offset + channel] != result;
            pixels[offset + channel] = result;
        }
    }
    return changed;
}
}
