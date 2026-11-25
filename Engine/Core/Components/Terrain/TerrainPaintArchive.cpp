#include "Core/Components/Terrain/TerrainPaintArchive.h"

#include <miniz.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace Engine::Components
{
namespace
{
constexpr uint64_t Magic = 0x41544e50524e5254ull; // TRNRPNTA
constexpr uint32_t Version = 1;

template<class T> void WriteValue(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}
template<class T> bool ReadValue(std::ifstream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}
uint8_t Quantize(float value)
{
    return static_cast<uint8_t>(std::clamp(std::lround(value * 255.f), 0l, 255l));
}
}

bool TerrainPaintArchive::Write(const std::string& path, const Header& header, const CellPaint& cellPaint,
    const std::function<void(size_t, size_t)>& progress)
{
    if (header.extent < 0 || header.cellsX < 1 || header.cellsY < 1 || header.layers < 1 || header.layers > 255)
        return false;
    const int side = header.extent * 2 + 1;
    const size_t cellCount = size_t(side) * side;
    const size_t vertices = size_t(header.cellsX + 1) * (header.cellsY + 1);
    const size_t layers = size_t(header.layers);
    std::vector<std::vector<uint8_t>> blocks(cellCount);
    std::vector<uint32_t> rawSizes(cellCount, 0);
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    const auto work = [&]
    {
        std::vector<float> weights, lushness;
        std::vector<uint8_t> raw;
        for (size_t index = next++; index < cellCount && !failed; index = next++)
        {
            const Cell cell{int(index % side) - header.extent, int(index / side) - header.extent};
            weights.clear();
            lushness.clear();
            if (cellPaint(cell, weights, lushness) && weights.size() == vertices * layers)
            {
                raw.assign(vertices * (layers + 1), 0);
                for (size_t v = 0; v < vertices; ++v)
                {
                    for (size_t layer = 0; layer < layers; ++layer)
                        raw[layer * vertices + v] = Quantize(weights[v * layers + layer]);
                    raw[layers * vertices + v] = lushness.size() == vertices ? Quantize(lushness[v]) : 0;
                }
                mz_ulong length = mz_compressBound(mz_ulong(raw.size()));
                auto& block = blocks[index];
                block.resize(length);
                if (mz_compress2(block.data(), &length, raw.data(), mz_ulong(raw.size()), 9) != MZ_OK)
                    failed = true;
                block.resize(length);
                rawSizes[index] = uint32_t(raw.size());
            }
            ++done;
        }
    };
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    if (progress)
    {
        // The calling thread reports while the workers run.
        std::thread own(work);
        size_t reported = SIZE_MAX;
        while (done < cellCount && !failed)
        {
            if (done != reported) progress(reported = done, cellCount);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        own.join();
    }
    else
        work();
    for (auto& thread : pool) thread.join();
    if (failed) return false;
    if (progress) progress(cellCount, cellCount);

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        WriteValue(out, Magic); WriteValue(out, Version);
        WriteValue(out, int32_t(header.extent)); WriteValue(out, int32_t(header.cellsX));
        WriteValue(out, int32_t(header.cellsY)); WriteValue(out, uint32_t(header.layers));
        uint64_t offset = uint64_t(out.tellp()) + cellCount * (sizeof(uint64_t) + 2 * sizeof(uint32_t));
        for (size_t i = 0; i < cellCount; ++i)
        {
            WriteValue(out, blocks[i].empty() ? uint64_t(0) : offset);
            WriteValue(out, uint32_t(blocks[i].size()));
            WriteValue(out, rawSizes[i]);
            offset += blocks[i].size();
        }
        for (const auto& block : blocks)
            out.write(reinterpret_cast<const char*>(block.data()), std::streamsize(block.size()));
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return !error;
}

std::shared_ptr<const TerrainPaintArchive> TerrainPaintArchive::Open(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    auto archive = std::make_shared<TerrainPaintArchive>();
    archive->m_path = path;
    uint64_t magic = 0;
    uint32_t version = 0, layers = 0;
    int32_t extent = 0, cellsX = 0, cellsY = 0;
    if (!ReadValue(in, magic) || magic != Magic || !ReadValue(in, version) || version != Version ||
        !ReadValue(in, extent) || extent < 0 || extent > 4096 || !ReadValue(in, cellsX) || cellsX < 1 ||
        cellsX > 4096 || !ReadValue(in, cellsY) || cellsY < 1 || cellsY > 4096 || !ReadValue(in, layers) ||
        layers < 1 || layers > 255)
        return nullptr;
    archive->m_header = {extent, cellsX, cellsY, int(layers)};
    const size_t side = size_t(extent) * 2 + 1;
    archive->m_entries.resize(side * side);
    for (auto& entry : archive->m_entries)
        if (!ReadValue(in, entry.offset) || !ReadValue(in, entry.stored) || !ReadValue(in, entry.raw)) return nullptr;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto time = std::filesystem::last_write_time(path, error);
    archive->m_signature = (uint64_t(size) * 0x9e3779b97f4a7c15ull) ^
        uint64_t(time.time_since_epoch().count());
    const size_t raw = size_t(cellsX + 1) * (cellsY + 1) * (layers + 1);
    for (const auto& entry : archive->m_entries)
        if (entry.stored && (entry.offset + entry.stored > size || entry.raw != raw)) return nullptr;
    return archive;
}

bool TerrainPaintArchive::Covers(Cell cell) const
{
    return std::abs(cell.first) <= m_header.extent && std::abs(cell.second) <= m_header.extent;
}

bool TerrainPaintArchive::ReadCell(Cell cell, std::vector<float>& weights, std::vector<float>& lushness) const
{
    weights.clear();
    lushness.clear();
    if (!Covers(cell)) return false;
    const size_t side = size_t(m_header.extent) * 2 + 1;
    const Entry& entry = m_entries[size_t(cell.second + m_header.extent) * side + size_t(cell.first + m_header.extent)];
    if (!entry.stored) return false;
    std::vector<uint8_t> stored(entry.stored), raw(entry.raw);
    {
        std::ifstream in(m_path, std::ios::binary);
        if (!in) return false;
        in.seekg(std::streamoff(entry.offset));
        if (!in.read(reinterpret_cast<char*>(stored.data()), entry.stored)) return false;
    }
    mz_ulong length = entry.raw;
    if (mz_uncompress(raw.data(), &length, stored.data(), entry.stored) != MZ_OK || length != entry.raw)
        return false;
    const size_t vertices = size_t(m_header.cellsX + 1) * (m_header.cellsY + 1);
    const size_t layers = size_t(m_header.layers);
    weights.resize(vertices * layers);
    lushness.resize(vertices);
    for (size_t v = 0; v < vertices; ++v)
    {
        float sum = 0.f;
        for (size_t layer = 0; layer < layers; ++layer)
            sum += weights[v * layers + layer] = raw[layer * vertices + v] / 255.f;
        if (sum > 0.f)
            for (size_t layer = 0; layer < layers; ++layer) weights[v * layers + layer] /= sum;
        lushness[v] = raw[layers * vertices + v] / 255.f;
    }
    return true;
}
}
