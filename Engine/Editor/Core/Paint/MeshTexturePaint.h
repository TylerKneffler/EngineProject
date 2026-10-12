#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine::Editor::Paint
{
struct ProjectedPaintVertex
{
    float x = 0.f;
    float y = 0.f;
    float depth = 0.f;
    float inverseW = 0.f;
    bool valid = false;
};

inline void CommitTexturePaintHistory(
    std::deque<std::vector<uint8_t>>& undo,
    std::deque<std::vector<uint8_t>>& redo,
    std::vector<uint8_t> before, size_t limit)
{
    if (limit > 0)
    {
        if (undo.size() >= limit) undo.pop_front();
        undo.push_back(std::move(before));
    }
    redo.clear();
}

inline bool StepTexturePaintPixels(std::vector<uint8_t>& current,
    std::deque<std::vector<uint8_t>>& undo,
    std::deque<std::vector<uint8_t>>& redo, bool forward)
{
    auto& source = forward ? redo : undo;
    auto& destination = forward ? undo : redo;
    if (source.empty()) return false;
    destination.push_back(std::move(current));
    current = std::move(source.back());
    source.pop_back();
    return true;
}

template<class Vertex>
std::optional<size_t> PickMeshTextureTexel(uint32_t width,
    uint32_t height, const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<ProjectedPaintVertex>& projected,
    float x, float y, int uvSet)
{
    if (!width || !height || vertices.size() != projected.size() ||
        (uvSet != 0 && uvSet != 1)) return std::nullopt;
    float nearest = std::numeric_limits<float>::infinity();
    std::optional<size_t> texel;
    const size_t count = (indices.empty() ? vertices.size() :
        indices.size()) / 3;
    for (size_t face = 0; face < count; ++face)
    {
        const uint32_t ia = indices.empty() ? static_cast<uint32_t>(face * 3)
            : indices[face * 3];
        const uint32_t ib = indices.empty() ? static_cast<uint32_t>(face * 3 + 1)
            : indices[face * 3 + 1];
        const uint32_t ic = indices.empty() ? static_cast<uint32_t>(face * 3 + 2)
            : indices[face * 3 + 2];
        if (ia >= vertices.size() || ib >= vertices.size() ||
            ic >= vertices.size()) continue;
        const auto& a = projected[ia];
        const auto& b = projected[ib];
        const auto& c = projected[ic];
        if (!a.valid || !b.valid || !c.valid) continue;
        const float denominator = (b.y - c.y) * (a.x - c.x) +
            (c.x - b.x) * (a.y - c.y);
        if (std::abs(denominator) < 1e-6f) continue;
        const float wa = ((b.y - c.y) * (x - c.x) +
            (c.x - b.x) * (y - c.y)) / denominator;
        const float wb = ((c.y - a.y) * (x - c.x) +
            (a.x - c.x) * (y - c.y)) / denominator;
        const float wc = 1.f - wa - wb;
        if (wa < 0.f || wb < 0.f || wc < 0.f) continue;
        const float depth = wa * a.depth + wb * b.depth + wc * c.depth;
        if (depth >= nearest) continue;
        const float pa = wa * a.inverseW;
        const float pb = wb * b.inverseW;
        const float pc = wc * c.inverseW;
        const float total = pa + pb + pc;
        if (total <= 0.f) continue;
        const float* ua = uvSet == 0 ? vertices[ia].uv : vertices[ia].uv1;
        const float* ub = uvSet == 0 ? vertices[ib].uv : vertices[ib].uv1;
        const float* uc = uvSet == 0 ? vertices[ic].uv : vertices[ic].uv1;
        const float u = (pa * ua[0] + pb * ub[0] + pc * uc[0]) / total;
        const float v = (pa * ua[1] + pb * ub[1] + pc * uc[1]) / total;
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        const uint32_t tx = std::min(width - 1,
            static_cast<uint32_t>((u - std::floor(u)) * width));
        const uint32_t ty = std::min(height - 1,
            static_cast<uint32_t>((v - std::floor(v)) * height));
        nearest = depth;
        texel = static_cast<size_t>(ty) * width + tx;
    }
    return texel;
}

// Rasterize the visible mesh surface under a screen-space brush. A texel may
// be covered by many screen samples or by both sides of a UV seam; applying
// the strongest sample once avoids darkening overlapped UVs within one stamp.
template<class Vertex>
bool StampMeshTexture(std::vector<uint8_t>& pixels, uint32_t width,
    uint32_t height, const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<ProjectedPaintVertex>& projected,
    float centerX, float centerY, float radius, float hardness,
    float opacity, const uint8_t color[4], bool erase, int uvSet = 0,
    int falloffMode = 2, const std::vector<uint8_t>* tip = nullptr,
    uint32_t tipWidth = 0, uint32_t tipHeight = 0,
    bool tipUseLuminance = false, bool alphaLock = false)
{
    if (!width || !height || pixels.size() !=
            static_cast<size_t>(width) * height * 4 ||
        vertices.size() != projected.size() ||
        !std::isfinite(centerX) || !std::isfinite(centerY) ||
        !std::isfinite(radius) || radius <= 0.f ||
        (uvSet != 0 && uvSet != 1)) return false;

    const int left = static_cast<int>(std::floor(centerX - radius));
    const int top = static_cast<int>(std::floor(centerY - radius));
    const int right = static_cast<int>(std::ceil(centerX + radius));
    const int bottom = static_cast<int>(std::ceil(centerY + radius));
    const int sampleWidth = right - left + 1;
    const int sampleHeight = bottom - top + 1;
    if (sampleWidth <= 0 || sampleHeight <= 0 ||
        static_cast<uint64_t>(sampleWidth) * sampleHeight > 1000000)
        return false;
    struct Sample { float depth = std::numeric_limits<float>::infinity();
        size_t texel = 0; float amount = 0.f; };
    std::vector<Sample> samples(static_cast<size_t>(sampleWidth) * sampleHeight);
    hardness = std::clamp(hardness, 0.f, 1.f);
    opacity = std::clamp(opacity, 0.f, 1.f);

    const size_t faceCount = indices.empty() ? vertices.size() / 3 :
        indices.size() / 3;
    for (size_t face = 0; face < faceCount; ++face)
    {
        const uint32_t ids[3]{ indices.empty() ? static_cast<uint32_t>(face * 3)
                : indices[face * 3],
            indices.empty() ? static_cast<uint32_t>(face * 3 + 1)
                : indices[face * 3 + 1],
            indices.empty() ? static_cast<uint32_t>(face * 3 + 2)
                : indices[face * 3 + 2] };
        if (ids[0] >= vertices.size() || ids[1] >= vertices.size() ||
            ids[2] >= vertices.size()) continue;
        const auto& a = projected[ids[0]];
        const auto& b = projected[ids[1]];
        const auto& c = projected[ids[2]];
        if (!a.valid || !b.valid || !c.valid) continue;
        const float denominator = (b.y - c.y) * (a.x - c.x) +
            (c.x - b.x) * (a.y - c.y);
        if (std::abs(denominator) < 1e-6f) continue;
        const int minX = std::max(left, static_cast<int>(std::floor(
            std::min({a.x, b.x, c.x}))));
        const int maxX = std::min(right, static_cast<int>(std::ceil(
            std::max({a.x, b.x, c.x}))));
        const int minY = std::max(top, static_cast<int>(std::floor(
            std::min({a.y, b.y, c.y}))));
        const int maxY = std::min(bottom, static_cast<int>(std::ceil(
            std::max({a.y, b.y, c.y}))));
        for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
        {
            const float sx = x + .5f, sy = y + .5f;
            const float distance = std::hypot(sx - centerX, sy - centerY) / radius;
            if (distance > 1.f) continue;
            const float wa = ((b.y - c.y) * (sx - c.x) +
                (c.x - b.x) * (sy - c.y)) / denominator;
            const float wb = ((c.y - a.y) * (sx - c.x) +
                (a.x - c.x) * (sy - c.y)) / denominator;
            const float wc = 1.f - wa - wb;
            if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) continue;
            const float depth = wa * a.depth + wb * b.depth + wc * c.depth;
            Sample& sample = samples[static_cast<size_t>(y - top) *
                sampleWidth + (x - left)];
            if (depth >= sample.depth) continue;
            const float pa = wa * a.inverseW;
            const float pb = wb * b.inverseW;
            const float pc = wc * c.inverseW;
            const float reciprocal = pa + pb + pc;
            if (reciprocal <= 0.f) continue;
            const auto& va = vertices[ids[0]];
            const auto& vb = vertices[ids[1]];
            const auto& vc = vertices[ids[2]];
            const float* ua = uvSet == 0 ? va.uv : va.uv1;
            const float* ub = uvSet == 0 ? vb.uv : vb.uv1;
            const float* uc = uvSet == 0 ? vc.uv : vc.uv1;
            const float u = (pa * ua[0] + pb * ub[0] + pc * uc[0]) / reciprocal;
            const float v = (pa * ua[1] + pb * ub[1] + pc * uc[1]) / reciprocal;
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            const uint32_t tx = std::min(width - 1,
                static_cast<uint32_t>((u - std::floor(u)) * width));
            const uint32_t ty = std::min(height - 1,
                static_cast<uint32_t>((v - std::floor(v)) * height));
            float falloff = distance <= hardness ? 1.f :
                hardness >= 1.f ? 0.f :
                (1.f - distance) / (1.f - hardness);
            if (falloffMode == 0) falloff = distance <= hardness ? 1.f : 0.f;
            else if (falloffMode == 2)
                falloff = falloff * falloff * (3.f - 2.f * falloff);
            if (tip && tipWidth && tipHeight && tip->size() >=
                    static_cast<size_t>(tipWidth) * tipHeight * 4)
            {
                const float brushU = (sx - centerX) / (radius * 2.f) + .5f;
                const float brushV = (sy - centerY) / (radius * 2.f) + .5f;
                const uint32_t tipX = std::min(tipWidth - 1,
                    static_cast<uint32_t>(std::clamp(brushU, 0.f, 1.f) * tipWidth));
                const uint32_t tipY = std::min(tipHeight - 1,
                    static_cast<uint32_t>(std::clamp(brushV, 0.f, 1.f) * tipHeight));
                const size_t offset = (static_cast<size_t>(tipY) *
                    tipWidth + tipX) * 4;
                falloff *= tipUseLuminance ?
                    1.f - ((*tip)[offset] + (*tip)[offset + 1] +
                        (*tip)[offset + 2]) / 765.f :
                    (*tip)[offset + 3] / 255.f;
            }
            sample = { depth, static_cast<size_t>(ty) * width + tx,
                opacity * falloff };
        }
    }

    std::unordered_map<size_t, float> coverage;
    for (const Sample& sample : samples)
        if (sample.amount > 0.f)
            coverage[sample.texel] = std::max(coverage[sample.texel],
                sample.amount);
    bool changed = false;
    for (const auto& [texel, amount] : coverage)
    {
        const size_t offset = texel * 4;
        const uint8_t before[4]{ pixels[offset], pixels[offset + 1],
            pixels[offset + 2], pixels[offset + 3] };
        if (alphaLock && (erase || before[3] == 0)) continue;
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
}
