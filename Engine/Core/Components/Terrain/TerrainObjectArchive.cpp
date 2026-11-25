#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Scene/Object.h"

#include <miniz.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace Engine::Components
{
namespace
{
constexpr uint64_t Magic = 0x414a424f524e5254ull; // TRNROBJA
constexpr uint32_t Version = 2;
constexpr size_t RecordBytes = 12;   // a placement over all columns

template<class T> void Put(std::vector<uint8_t>& out, const T& value)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
template<class T> bool Take(const std::vector<uint8_t>& in, size_t& at, T& value)
{
    if (at + sizeof(T) > in.size()) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
template<class T> void WriteValue(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}
template<class T> bool ReadValue(std::ifstream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}
void WriteString(std::ofstream& out, const std::string& text)
{
    WriteValue(out, uint32_t(text.size()));
    out.write(text.data(), std::streamsize(text.size()));
}
bool ReadString(std::ifstream& in, std::string& text, uint32_t limit)
{
    uint32_t length = 0;
    if (!ReadValue(in, length) || length > limit) return false;
    text.resize(length);
    return static_cast<bool>(in.read(text.data(), length));
}
}

bool TerrainObjectArchive::Write(const std::string& path, const Header& header,
    const std::vector<std::pair<std::string, JsonValue>>& templates,
    const std::function<std::vector<Layer>(Cell)>& cellLayers,
    const std::function<void(size_t, size_t)>& progress)
{
    const int side = header.extent * 2 + 1;
    const size_t cellCount = size_t(side) * side;
    std::map<std::string, uint16_t> layerIndex;
    for (size_t i = 0; i < templates.size(); ++i) layerIndex[templates[i].first] = uint16_t(i);
    std::map<std::string, uint16_t> assetIndex;
    std::vector<std::string> assets;
    std::vector<std::vector<uint8_t>> blocks(cellCount);
    std::vector<uint32_t> rawSizes(cellCount, 0);
    const float spanX = header.cellsX * header.cellWidth, spanZ = header.cellsY * header.cellDepth;
    for (int cz = -header.extent; cz <= header.extent; ++cz)
        for (int cx = -header.extent; cx <= header.extent; ++cx)
        {
            const size_t index = size_t(cz + header.extent) * side + size_t(cx + header.extent);
            const auto layers = cellLayers({cx, cz});
            std::vector<uint8_t> raw;
            uint16_t written = 0;
            Put(raw, uint16_t(0));
            for (const auto& layer : layers)
            {
                const auto found = layerIndex.find(layer.name);
                if (found == layerIndex.end() || layer.placements.empty()) continue;
                Put(raw, found->second);
                Put(raw, uint32_t(layer.placements.size()));
                struct Record { uint16_t asset, x, z; int16_t y; int8_t nx, nz; uint8_t yaw, scale; };
                std::vector<Record> records;
                records.reserve(layer.placements.size());
                for (const auto& placement : layer.placements)
                {
                    auto asset = assetIndex.find(placement.asset);
                    if (asset == assetIndex.end())
                    {
                        if (assets.size() >= 65535) return false;
                        asset = assetIndex.emplace(placement.asset, uint16_t(assets.size())).first;
                        assets.push_back(placement.asset);
                    }
                    const auto quantize = [](float value, float span) {
                        return uint16_t(std::clamp(std::lround(value / span * 65535.f), 0l, 65535l));
                    };
                    const glm::vec3 n = glm::length(placement.normal) > 1e-6f
                        ? glm::normalize(placement.normal) : glm::vec3(0.f, 1.f, 0.f);
                    float yaw = std::fmod(placement.yawDegrees, 360.f);
                    if (yaw < 0.f) yaw += 360.f;
                    records.push_back({asset->second, quantize(placement.position.x, spanX),
                        quantize(placement.position.z, spanZ),
                        int16_t(std::clamp(std::lround(placement.position.y * 16.f), -32768l, 32767l)),
                        int8_t(std::clamp(std::lround(n.x * 127.f), -127l, 127l)),
                        int8_t(std::clamp(std::lround(n.z * 127.f), -127l, 127l)),
                        uint8_t(std::lround(yaw / 360.f * 256.f) & 255),
                        uint8_t(std::clamp(std::lround(placement.scale * 40.f), 1l, 255l))});
                }
                std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
                    return a.asset != b.asset ? a.asset < b.asset : a.x < b.x;
                });
                for (const auto& r : records) Put(raw, r.asset);
                uint16_t previous = 0;
                for (const auto& r : records) { Put(raw, uint16_t(r.x - previous)); previous = r.x; }
                for (const auto& r : records) Put(raw, r.z);
                for (const auto& r : records) Put(raw, r.y);
                for (const auto& r : records) Put(raw, r.nx);
                for (const auto& r : records) Put(raw, r.nz);
                for (const auto& r : records) Put(raw, r.yaw);
                for (const auto& r : records) Put(raw, r.scale);
                ++written;
            }
            if (written)
            {
                std::memcpy(raw.data(), &written, sizeof(written));
                mz_ulong length = mz_compressBound(mz_ulong(raw.size()));
                blocks[index].resize(length);
                if (mz_compress2(blocks[index].data(), &length, raw.data(), mz_ulong(raw.size()), 9) != MZ_OK)
                    return false;
                blocks[index].resize(length);
                rawSizes[index] = uint32_t(raw.size());
            }
            if (progress) progress(index + 1, cellCount);
        }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        WriteValue(out, Magic); WriteValue(out, Version);
        WriteValue(out, int32_t(header.extent)); WriteValue(out, int32_t(header.cellsX)); WriteValue(out, int32_t(header.cellsY));
        WriteValue(out, header.cellWidth); WriteValue(out, header.cellDepth);
        WriteValue(out, uint32_t(templates.size()));
        for (const auto& [name, layer] : templates)
        {
            WriteString(out, name);
            WriteString(out, Engine::Serialization::AssetWriteBinary(layer));
        }
        WriteValue(out, uint32_t(assets.size()));
        for (const auto& asset : assets) WriteString(out, asset);
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

std::shared_ptr<const TerrainObjectArchive> TerrainObjectArchive::Open(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    auto archive = std::make_shared<TerrainObjectArchive>();
    archive->m_path = path;
    uint64_t magic = 0;
    uint32_t version = 0, layers = 0, assets = 0;
    int32_t extent = 0, cellsX = 0, cellsY = 0;
    auto& h = archive->m_header;
    if (!ReadValue(in, magic) || magic != Magic || !ReadValue(in, version) || version != Version ||
        !ReadValue(in, extent) || extent < 0 || extent > 4096 || !ReadValue(in, cellsX) || cellsX < 1 ||
        !ReadValue(in, cellsY) || cellsY < 1 || !ReadValue(in, h.cellWidth) || !(h.cellWidth > 0.f) ||
        !ReadValue(in, h.cellDepth) || !(h.cellDepth > 0.f) || !ReadValue(in, layers) || layers > 4096)
        return nullptr;
    h.extent = extent; h.cellsX = cellsX; h.cellsY = cellsY;
    for (uint32_t i = 0; i < layers; ++i)
    {
        std::string name, bytes;
        if (!ReadString(in, name, 4096) || !ReadString(in, bytes, 64u << 20)) return nullptr;
        try { archive->m_templates.emplace_back(name, Engine::Serialization::AssetParse(bytes)); }
        catch (...) { return nullptr; }
    }
    if (!ReadValue(in, assets) || assets > 65535) return nullptr;
    archive->m_assets.resize(assets);
    for (auto& asset : archive->m_assets)
        if (!ReadString(in, asset, 4096)) return nullptr;
    const size_t side = size_t(extent) * 2 + 1;
    archive->m_entries.resize(side * side);
    for (auto& entry : archive->m_entries)
        if (!ReadValue(in, entry.offset) || !ReadValue(in, entry.stored) || !ReadValue(in, entry.raw)) return nullptr;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto time = std::filesystem::last_write_time(path, error);
    archive->m_signature = (uint64_t(size) * 0x9e3779b97f4a7c15ull) ^
        uint64_t(time.time_since_epoch().count());
    for (const auto& entry : archive->m_entries)
        if (entry.stored && entry.offset + entry.stored > size) return nullptr;
    return archive;
}

bool TerrainObjectArchive::Covers(Cell cell) const
{
    return std::abs(cell.first) <= m_header.extent && std::abs(cell.second) <= m_header.extent;
}

const TerrainObjectArchive::JsonValue* TerrainObjectArchive::Template(const std::string& layerName) const
{
    for (const auto& [name, layer] : m_templates)
        if (name == layerName) return &layer;
    return nullptr;
}

bool TerrainObjectArchive::ReadCell(Cell cell, std::vector<Layer>& layers) const
{
    layers.clear();
    if (!Covers(cell)) return false;
    const size_t side = size_t(m_header.extent) * 2 + 1;
    const Entry& entry = m_entries[size_t(cell.second + m_header.extent) * side + size_t(cell.first + m_header.extent)];
    if (!entry.stored) return true;
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
    const float spanX = m_header.cellsX * m_header.cellWidth, spanZ = m_header.cellsY * m_header.cellDepth;
    size_t at = 0;
    uint16_t count = 0;
    if (!Take(raw, at, count)) return false;
    for (uint16_t i = 0; i < count; ++i)
    {
        uint16_t index = 0;
        uint32_t placements = 0;
        if (!Take(raw, at, index) || index >= m_templates.size() || !Take(raw, at, placements) ||
            at + size_t(placements) * RecordBytes > raw.size()) return false;
        Layer layer;
        layer.name = m_templates[index].first;
        layer.placements.resize(placements);
        // Columns: asset, x delta, z, y, normal x, normal z, yaw, scale.
        const size_t n = placements;
        const uint8_t* base = raw.data() + at;
        const auto u16 = [&](size_t column, size_t i) {
            uint16_t v; std::memcpy(&v, base + column + i * 2, 2); return v; };
        const size_t assetColumn = 0, xColumn = 2 * n, zColumn = 4 * n, yColumn = 6 * n;
        const size_t nxColumn = 8 * n, nzColumn = 9 * n, yawColumn = 10 * n, scaleColumn = 11 * n;
        uint16_t x = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const uint16_t asset = u16(assetColumn, i);
            if (asset >= m_assets.size()) return false;
            x = uint16_t(x + u16(xColumn, i));
            int16_t y; std::memcpy(&y, base + yColumn + i * 2, 2);
            const float fx = int8_t(base[nxColumn + i]) / 127.f, fz = int8_t(base[nzColumn + i]) / 127.f;
            auto& placement = layer.placements[i];
            placement.asset = m_assets[asset];
            placement.position = {x / 65535.f * spanX, y / 16.f, u16(zColumn, i) / 65535.f * spanZ};
            placement.normal = {fx, std::sqrt(std::max(0.f, 1.f - fx * fx - fz * fz)), fz};
            placement.yawDegrees = base[yawColumn + i] / 256.f * 360.f;
            if (placement.yawDegrees > 180.f) placement.yawDegrees -= 360.f;
            placement.scale = base[scaleColumn + i] / 40.f;
        }
        at += n * RecordBytes;
        layers.push_back(std::move(layer));
    }
    return true;
}

bool TerrainObjectArchive::ReadCellLayers(Cell cell, JsonValue& out) const
{
    std::vector<Layer> layers;
    if (!ReadCell(cell, layers)) return false;
    out = JsonValue::MakeArray();
    for (auto& layer : layers)
    {
        Engine::Core::Object object;
        auto* component = object.AddComponent<TerrainObjectLayer>();
        if (const auto* layerTemplate = Template(layer.name)) component->Deserialize(*layerTemplate);
        component->layerName = layer.name;
        component->placements = std::move(layer.placements);
        out.Push(component->Serialize());
    }
    return true;
}
}
