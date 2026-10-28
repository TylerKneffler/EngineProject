#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Fixed-size, random-access height samples for authored terrain maps.
// Samples use signed 1/256-meter units. Identical world-edge samples quantize
// identically in neighboring cells, so the archive preserves shared borders.
namespace TerrainHeightArchive
{
constexpr uint64_t Magic = 0x31544748524e5254ull; // TRNRHGT1
constexpr uint32_t Version = 2;
constexpr int LayerCount = 4;
constexpr float Scale = 256.f;
constexpr std::streamoff HeaderBytes = 36;
constexpr uint64_t FarGridMagic = 0x314452474e525254ull; // TRNRGRD1
constexpr uint32_t FarGridVersion = 2;

template<class T> bool Read(std::ifstream& file, T& value);

struct FarPatch
{
    int32_t x = 0, y = 0;
    std::vector<float> heights;
    // Optional live or edited RGB values; the on-disk far-grid stores heights.
    std::vector<float> colors;
};

// Read only the sculpt portion of a sparse terrain edit. Paint weights do not
// affect the distant silhouette.
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
        !Read(file, version) || version != 1 ||
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
    char trailing = 0;
    return !file.read(&trailing, 1) && file.eof();
}

inline bool ApplyEditHeights(const std::string& path, int cellsX, int cellsY,
    std::vector<float>& heights)
{
    return ApplyEditSurface(path, cellsX, cellsY, heights);
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

inline bool ReadSignature(const std::string& path, int expectedExtent,
    int expectedX, int expectedY, uint64_t& signature)
{
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0, savedSignature = 0;
    uint32_t version = 0;
    int extent = 0, cellsX = 0, cellsY = 0;
    float scale = 0.f;
    if (!file || !Read(file, magic) || magic != Magic ||
        !Read(file, version) || version != Version ||
        !Read(file, savedSignature) || !savedSignature ||
        !Read(file, extent) || extent != expectedExtent ||
        !Read(file, cellsX) || cellsX != expectedX ||
        !Read(file, cellsY) || cellsY != expectedY ||
        !Read(file, scale) || scale != Scale) return false;
    std::error_code error;
    if (std::filesystem::file_size(path, error) !=
        ExpectedBytes(extent, cellsX, cellsY) || error) return false;
    signature = savedSignature;
    return true;
}

inline int16_t Quantize(float height)
{
    return static_cast<int16_t>(std::clamp(std::lround(height * Scale),
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

// Reconstruct soft base paint from the saved height while retaining the
// archive's two-bit layer choice for any deliberately assigned base layer.
inline void BlendBaseLayers(float height, uint8_t savedLayer, float* weights)
{
    constexpr float boundaries[LayerCount - 1] = {-12.f, 0.f, 16.f};
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
    std::vector<float>* weights = nullptr)
{
    if (std::abs(cellX) > expectedExtent || std::abs(cellY) > expectedExtent)
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0;
    int extent = 0, cellsX = 0, cellsY = 0;
    float scale = 0.f;
    if (!Read(file, magic) || magic != Magic ||
        !Read(file, version) || version != Version ||
        !Read(file, signature) || signature != expectedSignature ||
        !Read(file, extent) || extent != expectedExtent ||
        !Read(file, cellsX) || cellsX != expectedX ||
        !Read(file, cellsY) || cellsY != expectedY ||
        !Read(file, scale) || scale != Scale)
        return false;
    const uint64_t side = static_cast<uint64_t>(extent) * 2 + 1;
    const uint64_t sampleCount = static_cast<uint64_t>(cellsX + 1) * (cellsY + 1);
    const uint64_t packedCount = (sampleCount + 3) / 4;
    const uint64_t cellIndex = static_cast<uint64_t>(cellY + extent) * side +
        static_cast<uint64_t>(cellX + extent);
    file.seekg(HeaderBytes + static_cast<std::streamoff>(
        cellIndex * (sampleCount * sizeof(int16_t) + packedCount)));
    if (!file) return false;
    std::vector<int16_t> samples(static_cast<size_t>(sampleCount));
    file.read(reinterpret_cast<char*>(samples.data()),
        static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
    if (!file) return false;
    heights.resize(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        heights[i] = static_cast<float>(samples[i]) / Scale;
    if (weights)
    {
        std::vector<uint8_t> packed(static_cast<size_t>(packedCount));
        file.read(reinterpret_cast<char*>(packed.data()),
            static_cast<std::streamsize>(packed.size()));
        if (!file) return false;
        weights->assign(samples.size() * LayerCount, 0.f);
        for (size_t i = 0; i < samples.size(); ++i)
            BlendBaseLayers(heights[i],
                (packed[i / 4] >> ((i % 4) * 2)) & 3u,
                weights->data() + i * LayerCount);
    }
    return true;
}

// One archived height at every cell corner. At 129 by 129 cells this is only
// 33,800 bytes of height data, and is enough for a distant static silhouette.
inline bool BakeFarGrid(const std::string& archivePath,
    const std::string& outputPath, uint64_t expectedSignature,
    int expectedExtent, int expectedX, int expectedY,
    const std::string& editDirectory = {},
    const std::vector<std::pair<int, int>>& editedCells = {})
{
    std::ifstream source(archivePath, std::ios::binary);
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0;
    int extent = 0, cellsX = 0, cellsY = 0;
    float scale = 0.f;
    if (!source || !Read(source, magic) || magic != Magic ||
        !Read(source, version) || version != Version ||
        !Read(source, signature) || signature != expectedSignature ||
        !Read(source, extent) || extent != expectedExtent ||
        !Read(source, cellsX) || cellsX != expectedX ||
        !Read(source, cellsY) || cellsY != expectedY ||
        !Read(source, scale) || scale != Scale) return false;
    const int cellSide = extent * 2 + 1;
    const int vertexSide = cellSide + 1;
    const size_t sampleCount = static_cast<size_t>(cellsX + 1) * (cellsY + 1);
    std::vector<int16_t> samples(sampleCount);
    std::vector<int16_t> grid(static_cast<size_t>(vertexSide) * vertexSide);
    const std::streamoff packedBytes = static_cast<std::streamoff>((sampleCount + 3) / 4);
    for (int row = 0; row < cellSide; ++row)
        for (int col = 0; col < cellSide; ++col)
        {
            source.read(reinterpret_cast<char*>(samples.data()),
                static_cast<std::streamsize>(sampleCount * sizeof(int16_t)));
            source.seekg(packedBytes, std::ios::cur);
            if (!source) return false;
            grid[static_cast<size_t>(row) * vertexSide + col] = samples[0];
            if (col == cellSide - 1)
                grid[static_cast<size_t>(row) * vertexSide + cellSide] =
                    samples[cellsX];
            if (row == cellSide - 1)
                grid[static_cast<size_t>(cellSide) * vertexSide + col] =
                    samples[static_cast<size_t>(cellsY) * (cellsX + 1)];
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
        for (float height : heights) patch.push_back(Quantize(height));
        patches.emplace_back(cell, std::move(patch));
    }
    const std::filesystem::path target(outputPath);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    const auto temporary = target.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !Write(output, FarGridMagic) ||
        !Write(output, FarGridVersion) || !Write(output, signature) ||
        !Write(output, extent) || !Write(output, vertexSide) ||
        !Write(output, cellsX) || !Write(output, cellsY)) return false;
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

inline bool ReadFarGrid(const std::string& path, uint64_t expectedSignature,
    int expectedExtent, std::vector<float>& heights,
    std::vector<FarPatch>* patches = nullptr)
{
    std::ifstream file(path, std::ios::binary);
    uint64_t magic = 0, signature = 0;
    uint32_t version = 0;
    int extent = 0, vertexSide = 0, cellsX = 0, cellsY = 0;
    if (!file || !Read(file, magic) || magic != FarGridMagic ||
        !Read(file, version) || version != FarGridVersion ||
        !Read(file, signature) || signature != expectedSignature ||
        !Read(file, extent) || extent != expectedExtent ||
        !Read(file, vertexSide) || vertexSide != extent * 2 + 2 ||
        !Read(file, cellsX) || cellsX < 1 || cellsX > 256 ||
        !Read(file, cellsY) || cellsY < 1 || cellsY > 256) return false;
    std::vector<int16_t> quantized(static_cast<size_t>(vertexSide) * vertexSide);
    file.read(reinterpret_cast<char*>(quantized.data()),
        static_cast<std::streamsize>(quantized.size() * sizeof(int16_t)));
    if (!file) return false;
    heights.resize(quantized.size());
    for (size_t index = 0; index < quantized.size(); ++index)
        heights[index] = static_cast<float>(quantized[index]) / Scale;
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
            patch.heights.push_back(static_cast<float>(sample) / Scale);
        loaded.push_back(std::move(patch));
    }
    char trailing = 0;
    if (file.read(&trailing, 1) || !file.eof()) return false;
    if (patches) *patches = std::move(loaded);
    return true;
}
}
