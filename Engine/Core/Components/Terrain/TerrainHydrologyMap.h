#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Erosion maps written by tools/build_landscape_demo.py next to the height
// archive: world.flow (water that passed each texel) and world.deposition
// (sediment laid down). Little endian, packed header:
//   u64 magic, u32 version 1, u64 archive signature, i32 width, i32 height,
//   f32 spacing (m), f32 origin x, f32 origin z (position of sample 0),
//   f32 value scale
// then width * height u16 samples, rows along +z.
//   flow:       expm1(s / 65535 * scale) droplet-steps
//   deposition: s / 65535 * scale        metres
struct TerrainHydrologyMap
{
    static constexpr uint64_t FlowMagic = 0x31574C46524E5254ull;       // "TRNRFLW1"
    static constexpr uint64_t DepositionMagic = 0x31504544524E5254ull; // "TRNRDEP1"

    int width = 0, height = 0;
    float spacing = 1.f, originX = 0.f, originZ = 0.f, scale = 0.f;
    std::vector<uint16_t> samples;

    bool Empty() const { return samples.empty(); }

    // expectedSignature 0 accepts any archive.
    bool Load(const std::string& path, uint64_t magic, uint64_t expectedSignature)
    {
        *this = {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        uint64_t fileMagic = 0, signature = 0;
        uint32_t version = 0;
        int32_t w = 0, h = 0;
        float values[4]{};
        const auto read = [&](auto& value) {
            return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
        };
        if (!read(fileMagic) || fileMagic != magic || !read(version) || version != 1 ||
            !read(signature) || (expectedSignature && signature != expectedSignature) ||
            !read(w) || !read(h) || w <= 0 || h <= 0 || w > 65536 || h > 65536)
            return false;
        for (float& value : values) if (!read(value) || !std::isfinite(value)) return false;
        if (values[0] <= 0.f) return false;
        std::vector<uint16_t> data(static_cast<size_t>(w) * h);
        if (!file.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(uint16_t))))
            return false;
        width = w; height = h;
        spacing = values[0]; originX = values[1]; originZ = values[2]; scale = values[3];
        samples = std::move(data);
        return true;
    }

    // Stored value at a world position, bilinear, 0..1 (0 outside the map).
    float Normalized(float x, float z) const
    {
        if (samples.empty()) return 0.f;
        const float gx = (x - originX) / spacing, gz = (z - originZ) / spacing;
        if (gx < 0.f || gz < 0.f || gx > float(width - 1) || gz > float(height - 1)) return 0.f;
        const int x0 = std::min(static_cast<int>(gx), width - 2 < 0 ? 0 : width - 2);
        const int z0 = std::min(static_cast<int>(gz), height - 2 < 0 ? 0 : height - 2);
        const int x1 = std::min(x0 + 1, width - 1), z1 = std::min(z0 + 1, height - 1);
        const float tx = gx - x0, tz = gz - z0;
        const auto at = [&](int sx, int sz) {
            return samples[static_cast<size_t>(sz) * width + sx] * (1.f / 65535.f);
        };
        return (at(x0, z0) * (1.f - tx) + at(x1, z0) * tx) * (1.f - tz) +
            (at(x0, z1) * (1.f - tx) + at(x1, z1) * tx) * tz;
    }
};
