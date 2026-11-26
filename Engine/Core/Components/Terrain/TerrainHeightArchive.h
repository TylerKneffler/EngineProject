#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Fixed-size, random-access height samples for authored terrain maps.
// Samples are signed fixed-point heights: 1/256 m in version 2, a scale from
// the header in version 3 (see Layout). Identical world-edge samples quantize
// identically in neighboring cells, so the archive preserves shared borders.
namespace TerrainHeightArchive
{
constexpr uint64_t Magic = 0x31544748524e5254ull; // TRNRHGT1
constexpr uint32_t Version = 2;
constexpr int LayerCount = 4;
constexpr float Scale = 256.f;
constexpr std::streamoff HeaderBytes = 36;
constexpr uint64_t FarGridMagic = 0x314452474e525254ull; // TRNRGRD1
constexpr uint32_t FarGridVersion = 3;

template<class T> bool Read(std::ifstream& file, T& value);

struct FarPatch
{
    int32_t x = 0, y = 0;
    std::vector<float> heights;
    // Optional live or edited RGB values; the on-disk far-grid stores heights.
    std::vector<float> colors;
};

// Read only the sculpt portion of a sparse terrain edit (.terrainedit, see
// TerrainManager::SaveEdit). Paint weights do not affect the distant
// silhouette; version 2's erosion entries are skipped.
inline bool ApplyEditSurface(const std::string& path, int cellsX, int cellsY,
    std::vector<float>& heights, std::vector<float>* weights = nullptr,
    uint32_t expectedLayers = 0)
{
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0;
    uint32_t version = 0, x = 0, y = 0, layers = 0, heightCount = 0;
    uint32_t weightCount = 0;
    const size_t sampleCount = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    if (!file || heights.size() != sampleCount ||
        !Read(file, magic) || magic != 0x31544944454e5254ull ||
        !Read(file, version) || version < 1 || version > 3 ||
        !Read(file, x) || x != static_cast<uint32_t>(cellsX) ||
        !Read(file, y) || y != static_cast<uint32_t>(cellsY) ||
        !Read(file, layers) || layers > 6 ||
        !Read(file, heightCount) || heightCount > sampleCount ||
        !Read(file, weightCount) || weightCount > sampleCount) return false;
    for (uint32_t n = 0; n < heightCount; ++n)
    {
        uint32_t index = 0;
        float height = 0.f;
        if (!Read(file, index) || index >= sampleCount ||
            !Read(file, height) || !std::isfinite(height)) return false;
        heights[index] = height;
    }
    for (uint32_t n = 0; n < weightCount; ++n)
    {
        uint32_t index = 0;
        if (!Read(file, index) || index >= sampleCount) return false;
        for (uint32_t layer = 0; layer < layers; ++layer)
        {
            float weight = 0.f;
            if (!Read(file, weight)) return false;
            if (weights && layers == expectedLayers &&
                weights->size() == sampleCount * expectedLayers)
                (*weights)[static_cast<size_t>(index) * expectedLayers + layer] =
                    weight;
        }
    }
    if (version == 2)
    {
        uint32_t erosionCount = 0;
        if (!Read(file, erosionCount) || erosionCount > sampleCount) return false;
        // index, flow, deposition
        file.seekg(static_cast<std::streamoff>(erosionCount) * 12, std::ios::cur);
        if (!file) return false;
    }
    char trailing = 0;
    return !file.read(&trailing, 1) && file.eof();
}

inline bool ApplyEditHeights(const std::string& path, int cellsX, int cellsY,
    std::vector<float>& heights)
{
    return ApplyEditSurface(path, cellsX, cellsY, heights);
}

// Everything a sparse terrain edit holds, as stored: for tools that need a
// cell's edits without loading the cell (a macro color patch).
struct EditEntries
{
    uint32_t layers = 0;
    std::vector<std::pair<uint32_t, float>> heights;
    std::vector<std::pair<uint32_t, std::vector<float>>> weights;   // `layers` each
};
inline bool ReadEditEntries(const std::string& path, int cellsX, int cellsY, EditEntries& entries)
{
    entries = {};
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0;
    uint32_t version = 0, x = 0, y = 0, heightCount = 0, weightCount = 0;
    const size_t sampleCount = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    if (!file || !Read(file, magic) || magic != 0x31544944454e5254ull ||
        !Read(file, version) || version < 1 || version > 3 ||
        !Read(file, x) || x != static_cast<uint32_t>(cellsX) ||
        !Read(file, y) || y != static_cast<uint32_t>(cellsY) ||
        !Read(file, entries.layers) || entries.layers > 6 ||
        !Read(file, heightCount) || heightCount > sampleCount ||
        !Read(file, weightCount) || weightCount > sampleCount) return false;
    for (uint32_t n = 0; n < heightCount; ++n)
    {
        std::pair<uint32_t, float> entry;
        if (!Read(file, entry.first) || entry.first >= sampleCount ||
            !Read(file, entry.second) || !std::isfinite(entry.second)) return false;
        entries.heights.push_back(entry);
    }
    for (uint32_t n = 0; n < weightCount; ++n)
    {
        std::pair<uint32_t, std::vector<float>> entry;
        entry.second.resize(entries.layers);
        if (!Read(file, entry.first) || entry.first >= sampleCount) return false;
        for (float& weight : entry.second) if (!Read(file, weight)) return false;
        entries.weights.push_back(std::move(entry));
    }
    // Version 2's erosion entries (index, flow, deposition): skipped.
    if (version == 2)
    {
        uint32_t erosionCount = 0;
        if (!Read(file, erosionCount) || erosionCount > sampleCount) return false;
        file.seekg(static_cast<std::streamoff>(erosionCount) * 12, std::ios::cur);
        if (!file) return false;
    }
    char trailing = 0;
    return !file.read(&trailing, 1) && file.eof();
}

inline uint64_t ExpectedBytes(int extent, int cellsX, int cellsY)
{
    const uint64_t side = static_cast<uint64_t>(extent) * 2 + 1;
    const uint64_t samples = static_cast<uint64_t>(cellsX + 1) * (cellsY + 1);
    return HeaderBytes + side * side * (samples * sizeof(int16_t) +
        (samples + 3) / 4);
}

template<class T> bool Write(std::ofstream& file, const T& value)
{
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}
template<class T> bool Read(std::ifstream& file, T& value)
{
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}

inline bool BeginWrite(std::ofstream& file, uint64_t signature,
    int extent, int cellsX, int cellsY)
{
    return Write(file, Magic) && Write(file, Version) &&
        Write(file, signature) && Write(file, extent) &&
        Write(file, cellsX) && Write(file, cellsY) && Write(file, Scale);
}

// Archive layout from a file header. Version 2 stores every cell at full
// resolution with Scale. Version 3 stores its own scale (so tall maps can trade
// precision for range) and two resolutions: cells within fineHalfExtent of the
// origin (Chebyshev distance) at full resolution, the rest at every
// coarseStride-th sample, upsampled on read (see Upsample).
struct Layout
{
    uint32_t version = 0;
    uint64_t signature = 0;
    int extent = 0, cellsX = 0, cellsY = 0;
    float scale = 0.f;
    int fineHalfExtent = 0, coarseStride = 1;
    std::streamoff headerBytes = HeaderBytes;

    int Stride(int cellX, int cellY) const
    {
        return std::max(std::abs(cellX), std::abs(cellY)) <= fineHalfExtent
            ? 1 : coarseStride;
    }
    static uint64_t StoredSamples(int cellsX, int cellsY, int stride)
    {
        return static_cast<uint64_t>(cellsX / stride + 1) * (cellsY / stride + 1);
    }
    uint64_t CellBytes(int stride) const
    {
        const uint64_t samples = StoredSamples(cellsX, cellsY, stride);
        return samples * sizeof(int16_t) + (samples + 3) / 4;
    }
    // Fine cells in one row of cells.
    int FineInRow(int cellY) const
    {
        if (std::abs(cellY) > fineHalfExtent) return 0;
        return std::min(fineHalfExtent, extent) * 2 + 1;
    }
    std::streamoff CellOffset(int cellX, int cellY) const
    {
        const int side = extent * 2 + 1;
        const uint64_t fineBytes = CellBytes(1), coarseBytes = CellBytes(coarseStride);
        uint64_t offset = 0;
        for (int y = -extent; y < cellY; ++y)
        {
            const uint64_t fine = static_cast<uint64_t>(FineInRow(y));
            offset += fine * fineBytes + (side - fine) * coarseBytes;
        }
        for (int x = -extent; x < cellX; ++x)
            offset += Stride(x, cellY) == 1 ? fineBytes : coarseBytes;
        return headerBytes + static_cast<std::streamoff>(offset);
    }
    uint64_t TotalBytes() const
    {
        return static_cast<uint64_t>(CellOffset(-extent, extent + 1));
    }
};

// Reads and validates a header; expected values of 0 are not checked.
inline bool ReadLayout(std::ifstream& file, Layout& layout,
    uint64_t expectedSignature, int expectedExtent, int expectedX, int expectedY)
{
    uint64_t magic = 0;
    if (!file || !Read(file, magic) || magic != Magic ||
        !Read(file, layout.version) || (layout.version != 2 && layout.version != 3) ||
        !Read(file, layout.signature) ||
        (expectedSignature && layout.signature != expectedSignature) ||
        !Read(file, layout.extent) || layout.extent != expectedExtent ||
        !Read(file, layout.cellsX) || layout.cellsX != expectedX ||
        !Read(file, layout.cellsY) || layout.cellsY != expectedY ||
        !Read(file, layout.scale) || !std::isfinite(layout.scale) ||
        layout.scale <= 0.f) return false;
    layout.fineHalfExtent = layout.extent;
    layout.coarseStride = 1;
    layout.headerBytes = HeaderBytes;
    if (layout.version == 2) return layout.scale == Scale;
    if (!Read(file, layout.fineHalfExtent) || layout.fineHalfExtent < 0 ||
        !Read(file, layout.coarseStride) || layout.coarseStride < 1 ||
        layout.coarseStride > layout.cellsX || layout.coarseStride > layout.cellsY ||
        layout.cellsX % layout.coarseStride || layout.cellsY % layout.coarseStride)
        return false;
    layout.headerBytes = HeaderBytes + 2 * sizeof(int32_t);
    return true;
}

inline int64_t FloorDivide(int64_t value, int64_t divisor)
{
    const int64_t quotient = value / divisor;
    return quotient - ((value % divisor != 0) && ((value < 0) != (divisor < 0)));
}

// Integer bilinear upsampling of one coarse cell, rounded half up, so the
// generator can reproduce it exactly (shared edges between coarse and fine
// cells must quantize identically). Codes take the nearest coarse sample.
inline void Upsample(const std::vector<int16_t>& coarse, const std::vector<uint8_t>& coarseCodes,
    int cellsX, int cellsY, int stride, std::vector<int16_t>& samples,
    std::vector<uint8_t>& codes)
{
    const int width = cellsX / stride + 1, depth = cellsY / stride + 1;
    samples.resize(static_cast<size_t>(cellsX + 1) * (cellsY + 1));
    codes.resize(samples.size());
    const int64_t area = static_cast<int64_t>(stride) * stride;
    for (int row = 0; row <= cellsY; ++row)
    {
        const int y0 = std::min(row / stride, depth - 2), fy = row - y0 * stride;
        const int nearY = std::min((2 * row + stride) / (2 * stride), depth - 1);
        for (int col = 0; col <= cellsX; ++col)
        {
            const int x0 = std::min(col / stride, width - 2), fx = col - x0 * stride;
            const int nearX = std::min((2 * col + stride) / (2 * stride), width - 1);
            const auto at = [&](int x, int y) {
                return static_cast<int64_t>(coarse[static_cast<size_t>(y) * width + x]);
            };
            const int64_t sum = at(x0, y0) * (stride - fx) * (stride - fy) +
                at(x0 + 1, y0) * fx * (stride - fy) +
                at(x0, y0 + 1) * (stride - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
            const size_t index = static_cast<size_t>(row) * (cellsX + 1) + col;
            samples[index] = static_cast<int16_t>(FloorDivide(2 * sum + area, 2 * area));
            codes[index] = coarseCodes[static_cast<size_t>(nearY) * width + nearX];
        }
    }
}

// Full-resolution samples and two-bit codes of one cell, still quantized.
inline bool ReadCellSamples(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY, int cellX, int cellY,
    std::vector<int16_t>& samples, std::vector<uint8_t>& codes, float& scale)
{
    if (std::abs(cellX) > expectedExtent || std::abs(cellY) > expectedExtent)
        return false;
    std::ifstream file(path, std::ios::binary);
    Layout layout;
    if (!ReadLayout(file, layout, expectedSignature, expectedExtent, expectedX, expectedY))
        return false;
    const int stride = layout.Stride(cellX, cellY);
    const uint64_t count = Layout::StoredSamples(expectedX, expectedY, stride);
    file.seekg(layout.CellOffset(cellX, cellY));
    std::vector<int16_t> stored(static_cast<size_t>(count));
    std::vector<uint8_t> packed(static_cast<size_t>((count + 3) / 4));
    file.read(reinterpret_cast<char*>(stored.data()),
        static_cast<std::streamsize>(stored.size() * sizeof(int16_t)));
    file.read(reinterpret_cast<char*>(packed.data()),
        static_cast<std::streamsize>(packed.size()));
    if (!file) return false;
    std::vector<uint8_t> storedCodes(stored.size());
    for (size_t i = 0; i < stored.size(); ++i)
        storedCodes[i] = (packed[i / 4] >> ((i % 4) * 2)) & 3u;
    scale = layout.scale;
    if (stride == 1)
    {
        samples = std::move(stored);
        codes = std::move(storedCodes);
    }
    else
        Upsample(stored, storedCodes, expectedX, expectedY, stride, samples, codes);
    return true;
}

inline bool ReadSignature(const std::string& path, int expectedExtent,
    int expectedX, int expectedY, uint64_t& signature)
{
    std::ifstream file(path, std::ios::binary);
    Layout layout;
    if (!ReadLayout(file, layout, 0, expectedExtent, expectedX, expectedY) || !layout.signature)
        return false;
    // The size check: the file ends after the last cell.
    file.clear();
    file.seekg(0, std::ios::end);
    if (static_cast<uint64_t>(file.tellg()) != layout.TotalBytes()) return false;
    signature = layout.signature;
    return true;
}

inline int16_t Quantize(float height, float scale = Scale)
{
    return static_cast<int16_t>(std::clamp(std::lround(height * scale),
        static_cast<long>(std::numeric_limits<int16_t>::min()),
        static_cast<long>(std::numeric_limits<int16_t>::max())));
}

inline uint8_t LayerForHeight(float height)
{
    constexpr float minimum[LayerCount] = {-1000.f, -12.f, 0.f, 16.f};
    constexpr float maximum[LayerCount] = {-12.f, 0.f, 16.f, 1000.f};
    uint8_t nearest = 0;
    float distance = std::numeric_limits<float>::infinity();
    for (uint8_t layer = 0; layer < LayerCount; ++layer)
    {
        const float delta = std::max(minimum[layer] - height,
            height - maximum[layer]);
        if (delta < distance) { distance = delta; nearest = layer; }
    }
    return nearest;
}

template<size_t N>
inline void BlendHeightBands(float height,
    const std::array<float, N>& boundaries,
    float halfWidth, float* weights)
{
    float remaining = 1.f;
    for (size_t layer = 0; layer < N; ++layer)
    {
        const float t = std::clamp((height - boundaries[layer] + halfWidth) /
            (2.f * halfWidth), 0.f, 1.f);
        const float above = t * t * (3.f - 2.f * t);
        weights[layer] = remaining * (1.f - above);
        remaining *= above;
    }
    weights[N] = remaining;
}

// Reconstruct soft base paint from the saved height while retaining the
// archive's two-bit layer choice for any deliberately assigned base layer.
inline void BlendBaseLayers(float height, uint8_t savedLayer, float* weights,
    const std::array<float, LayerCount - 1>& boundaries = {-12.f, 0.f, 16.f})
{
    constexpr float halfWidth = 8.f;
    for (int layer = 0; layer < LayerCount; ++layer) weights[layer] = 0.f;
    if (savedLayer != LayerForHeight(height))
    {
        weights[savedLayer] = 1.f;
        return;
    }
    float above[LayerCount - 1];
    for (int boundary = 0; boundary < LayerCount - 1; ++boundary)
    {
        const float t = std::clamp((height - boundaries[boundary] + halfWidth) /
            (2.f * halfWidth), 0.f, 1.f);
        above[boundary] = t * t * (3.f - 2.f * t);
    }
    float remaining = 1.f;
    for (int layer = 0; layer < LayerCount - 1; ++layer)
    {
        weights[layer] = remaining * (1.f - above[layer]);
        remaining *= above[layer];
    }
    weights[LayerCount - 1] = remaining;
}

inline bool ReadCell(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY,
    int cellX, int cellY, std::vector<float>& heights,
    std::vector<float>* weights = nullptr,
    const std::array<float, LayerCount - 1>& baseBoundaries =
        {-12.f, 0.f, 16.f})
{
    std::vector<int16_t> samples;
    std::vector<uint8_t> codes;
    float scale = Scale;
    if (!ReadCellSamples(path, expectedSignature, expectedExtent, expectedX,
            expectedY, cellX, cellY, samples, codes, scale)) return false;
    heights.resize(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        heights[i] = static_cast<float>(samples[i]) / scale;
    if (weights)
    {
        weights->assign(samples.size() * LayerCount, 0.f);
        for (size_t i = 0; i < samples.size(); ++i)
            BlendBaseLayers(heights[i], codes[i],
                weights->data() + i * LayerCount, baseBoundaries);
    }
    return true;
}

// Heights plus the raw two-bit code stored with every sample. Maps whose
// codes are not base paint layers (for example a rock class computed over
// the whole map, so it matches across cell borders) read them with this.
inline bool ReadCellCodes(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY,
    int cellX, int cellY, std::vector<float>& heights,
    std::vector<uint8_t>& codes)
{
    std::vector<int16_t> samples;
    float scale = Scale;
    if (!ReadCellSamples(path, expectedSignature, expectedExtent, expectedX,
            expectedY, cellX, cellY, samples, codes, scale)) return false;
    heights.resize(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        heights[i] = static_cast<float>(samples[i]) / scale;
    return true;
}

// An archive's extent and signature from its header alone (any extent; the
// cell size must match). False when the file is missing or not an archive.
inline bool ReadHeader(const std::string& path, int expectedX, int expectedY, int& extent, uint64_t& signature)
{
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0;
    uint32_t version = 0;
    int cellsX = 0, cellsY = 0;
    if (!file || !Read(file, magic) || magic != Magic || !Read(file, version) ||
        (version != 2 && version != 3) || !Read(file, signature) || !Read(file, extent) ||
        !Read(file, cellsX) || !Read(file, cellsY) || cellsX != expectedX || cellsY != expectedY ||
        extent < 0 || !signature) return false;
    return true;
}

// The far terrain's grid straight from the archive: the height at every cell
// corner, (2 extent + 2)^2 values row-major from cell (-extent, -extent) (as
// a far grid file holds them, without patches). One pass over the file,
// reading each cell's stored corner samples; a full-resolution cell's corners
// win where it meets a coarse one (they are equal, by the generator's blend).
inline bool ReadCornerGrid(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY, std::vector<float>& heights)
{
    std::ifstream file(path, std::ios::binary);
    Layout layout;
    if (!ReadLayout(file, layout, expectedSignature, expectedExtent, expectedX, expectedY))
        return false;
    file.seekg(layout.headerBytes);
    const int side = layout.extent * 2 + 2;
    std::vector<int16_t> corners(static_cast<size_t>(side) * side, 0);
    std::vector<uint8_t> fine(corners.size(), 0);
    std::vector<int16_t> stored;
    for (int cellY = -layout.extent; cellY <= layout.extent; ++cellY)
        for (int cellX = -layout.extent; cellX <= layout.extent; ++cellX)
        {
            const int stride = layout.Stride(cellX, cellY);
            const int width = layout.cellsX / stride + 1, depth = layout.cellsY / stride + 1;
            stored.resize(static_cast<size_t>(width) * depth);
            file.read(reinterpret_cast<char*>(stored.data()),
                static_cast<std::streamsize>(stored.size() * sizeof(int16_t)));
            file.seekg(static_cast<std::streamoff>((stored.size() + 3) / 4), std::ios::cur);
            if (!file) return false;
            const int row = cellY + layout.extent, col = cellX + layout.extent;
            const auto set = [&](int r, int c, int16_t value) {
                const size_t index = static_cast<size_t>(r) * side + c;
                if (fine[index] && stride != 1) return;
                corners[index] = value;
                fine[index] = stride == 1 ? 1 : fine[index];
            };
            set(row, col, stored[0]);
            set(row, col + 1, stored[static_cast<size_t>(width) - 1]);
            set(row + 1, col, stored[static_cast<size_t>(depth - 1) * width]);
            set(row + 1, col + 1, stored.back());
        }
    heights.resize(corners.size());
    for (size_t i = 0; i < corners.size(); ++i) heights[i] = static_cast<float>(corners[i]) / layout.scale;
    return true;
}

// Visits every cell in file order with one open file: fn(cellX, cellY,
// heights, codes), both at full resolution. Stops (returns false) when fn
// returns false or the file is short.
template<class Fn>
inline bool ForEachCell(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY, Fn&& fn)
{
    std::ifstream file(path, std::ios::binary);
    Layout layout;
    if (!ReadLayout(file, layout, expectedSignature, expectedExtent, expectedX, expectedY))
        return false;
    file.seekg(layout.headerBytes);
    std::vector<int16_t> stored, samples;
    std::vector<uint8_t> packed, storedCodes, codes;
    std::vector<float> heights;
    for (int cellY = -layout.extent; cellY <= layout.extent; ++cellY)
        for (int cellX = -layout.extent; cellX <= layout.extent; ++cellX)
        {
            const int stride = layout.Stride(cellX, cellY);
            const uint64_t count = Layout::StoredSamples(expectedX, expectedY, stride);
            stored.resize(static_cast<size_t>(count));
            packed.resize(static_cast<size_t>((count + 3) / 4));
            file.read(reinterpret_cast<char*>(stored.data()),
                static_cast<std::streamsize>(stored.size() * sizeof(int16_t)));
            file.read(reinterpret_cast<char*>(packed.data()),
                static_cast<std::streamsize>(packed.size()));
            if (!file) return false;
            storedCodes.resize(stored.size());
            for (size_t i = 0; i < stored.size(); ++i)
                storedCodes[i] = (packed[i / 4] >> ((i % 4) * 2)) & 3u;
            if (stride == 1) { samples = stored; codes = storedCodes; }
            else Upsample(stored, storedCodes, expectedX, expectedY, stride, samples, codes);
            heights.resize(samples.size());
            for (size_t i = 0; i < samples.size(); ++i)
                heights[i] = static_cast<float>(samples[i]) / layout.scale;
            if (!fn(cellX, cellY, heights, codes)) return false;
        }
    return true;
}

// One archived height at every cell corner. At 129 by 129 cells this is only
// 33,800 bytes of height data, and is enough for a distant static silhouette.
// The grid keeps the archive's scale.
inline bool BakeFarGrid(const std::string& archivePath,
    const std::string& outputPath, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY,
    const std::string& editDirectory = {},
    const std::vector<std::pair<int, int>>& editedCells = {})
{
    std::ifstream source(archivePath, std::ios::binary);
    Layout layout;
    if (!ReadLayout(source, layout, expectedSignature, expectedExtent,
            expectedX, expectedY)) return false;
    const int extent = layout.extent, cellsX = layout.cellsX, cellsY = layout.cellsY;
    const int cellSide = extent * 2 + 1;
    const int vertexSide = cellSide + 1;
    std::vector<int16_t> grid(static_cast<size_t>(vertexSide) * vertexSide);
    std::vector<int16_t> samples;
    // Cell corners are stored samples at either resolution.
    for (int row = 0; row < cellSide; ++row)
        for (int col = 0; col < cellSide; ++col)
        {
            const int stride = layout.Stride(col - extent, row - extent);
            const int width = cellsX / stride + 1, depth = cellsY / stride + 1;
            samples.resize(static_cast<size_t>(width) * depth);
            source.seekg(layout.CellOffset(col - extent, row - extent));
            source.read(reinterpret_cast<char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
            if (!source) return false;
            grid[static_cast<size_t>(row) * vertexSide + col] = samples[0];
            if (col == cellSide - 1)
                grid[static_cast<size_t>(row) * vertexSide + cellSide] =
                    samples[width - 1];
            if (row == cellSide - 1)
                grid[static_cast<size_t>(cellSide) * vertexSide + col] =
                    samples[static_cast<size_t>(depth - 1) * width];
            if (row == cellSide - 1 && col == cellSide - 1)
                grid.back() = samples.back();
        }
    std::vector<std::pair<std::pair<int, int>, std::vector<int16_t>>> patches;
    for (const auto& cell : editedCells)
    {
        if (std::abs(cell.first) > extent || std::abs(cell.second) > extent)
            return false;
        std::vector<float> heights;
        if (!ReadCell(archivePath, expectedSignature, extent, cellsX, cellsY,
            cell.first, cell.second, heights)) return false;
        const auto editPath = std::filesystem::path(editDirectory) /
            (std::to_string(cell.first) + "_" + std::to_string(cell.second) +
                ".terrainedit");
        if (!ApplyEditHeights(editPath.string(), cellsX, cellsY, heights))
            return false;
        std::vector<int16_t> patch;
        patch.reserve(heights.size());
        for (float height : heights) patch.push_back(Quantize(height, layout.scale));
        patches.emplace_back(cell, std::move(patch));
    }
    const std::filesystem::path target(outputPath);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const auto temporary = target.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !Write(output, FarGridMagic) ||
        !Write(output, FarGridVersion) || !Write(output, layout.signature) ||
        !Write(output, extent) || !Write(output, vertexSide) ||
        !Write(output, cellsX) || !Write(output, cellsY) ||
        !Write(output, layout.scale)) return false;
    output.write(reinterpret_cast<const char*>(grid.data()),
        static_cast<std::streamsize>(grid.size() * sizeof(int16_t)));
    const uint32_t patchCount = static_cast<uint32_t>(patches.size());
    if (!Write(output, patchCount)) return false;
    for (const auto& patch : patches)
    {
        const int32_t x = patch.first.first, y = patch.first.second;
        if (!Write(output, x) || !Write(output, y)) return false;
        output.write(reinterpret_cast<const char*>(patch.second.data()),
            static_cast<std::streamsize>(patch.second.size() * sizeof(int16_t)));
        if (!output) return false;
    }
    output.close();
    if (!output) return false;
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    return !error;
}

// Version 2 grids use Scale; version 3 adds its own scale after cellsY.
inline bool ReadFarGrid(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, std::vector<float>& heights,
    std::vector<FarPatch>* patches = nullptr)
{
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0;
    int extent = 0, vertexSide = 0, cellsX = 0, cellsY = 0;
    float scale = Scale;
    if (!file || !Read(file, magic) || magic != FarGridMagic ||
        !Read(file, version) || (version != 2 && version != FarGridVersion) ||
        !Read(file, signature) || signature != expectedSignature ||
        !Read(file, extent) || extent != expectedExtent ||
        !Read(file, vertexSide) || vertexSide != extent * 2 + 2 ||
        !Read(file, cellsX) || cellsX < 1 || cellsX > 256 ||
        !Read(file, cellsY) || cellsY < 1 || cellsY > 256) return false;
    if (version >= 3 && (!Read(file, scale) || !std::isfinite(scale) || scale <= 0.f))
        return false;
    std::vector<int16_t> quantized(static_cast<size_t>(vertexSide) * vertexSide);
    file.read(reinterpret_cast<char*>(quantized.data()),
        static_cast<std::streamsize>(quantized.size() * sizeof(int16_t)));
    if (!file) return false;
    heights.resize(quantized.size());
    for (size_t index = 0; index < quantized.size(); ++index)
        heights[index] = static_cast<float>(quantized[index]) / scale;
    uint32_t patchCount = 0;
    if (!Read(file, patchCount) || patchCount >
        static_cast<uint32_t>(vertexSide - 1) * (vertexSide - 1)) return false;
    std::vector<FarPatch> loaded;
    for (uint32_t n = 0; n < patchCount; ++n)
    {
        FarPatch patch;
        if (!Read(file, patch.x) || !Read(file, patch.y) ||
            std::abs(patch.x) > extent || std::abs(patch.y) > extent) return false;
        std::vector<int16_t> samples(static_cast<size_t>(cellsX + 1) *
            (cellsY + 1));
        file.read(reinterpret_cast<char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
        if (!file) return false;
        patch.heights.reserve(samples.size());
        for (int16_t sample : samples)
            patch.heights.push_back(static_cast<float>(sample) / scale);
        loaded.push_back(std::move(patch));
    }
    char trailing = 0;
    if (file.read(&trailing, 1) || !file.eof()) return false;
    if (patches) *patches = std::move(loaded);
    return true;
}
}
