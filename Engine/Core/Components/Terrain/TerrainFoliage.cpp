#include "Core/Components/Terrain/TerrainFoliage.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Engine::Components
{
namespace
{
constexpr uint64_t FoliageMagic = 0x314c4f46524e5254ull; // "TRNRFOL1"
constexpr uint32_t FoliageVersion = 1;

int FloorDiv(int value, int divisor)
{
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

template <typename T> void Put(std::vector<uint8_t>& out, const T& value)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T> bool Get(const std::vector<uint8_t>& in, size_t& at, T& value)
{
    if (at + sizeof(T) > in.size()) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
}

bool TerrainFoliageCell::Empty() const
{
    for (const auto& [name, layer] : layers)
        if (!layer.chunks.empty()) return false;
    return true;
}

const TerrainFoliageCell::Layer* TerrainFoliageCell::Find(const std::string& layer) const
{
    const auto found = layers.find(layer);
    return found != layers.end() && !found->second.chunks.empty() ? &found->second : nullptr;
}

glm::ivec3 TerrainFoliageCell::VoxelOf(const glm::vec3& local)
{
    return glm::ivec3(glm::floor(local / VoxelSize));
}

glm::vec3 TerrainFoliageCell::VoxelCentre(const glm::ivec3& voxel)
{
    return (glm::vec3(voxel) + .5f) * VoxelSize;
}

TerrainFoliageCell::ChunkKey TerrainFoliageCell::ChunkOf(const glm::ivec3& voxel)
{
    return {static_cast<int16_t>(FloorDiv(voxel.x, ChunkSide)),
        static_cast<int16_t>(FloorDiv(voxel.y, ChunkSide)),
        static_cast<int16_t>(FloorDiv(voxel.z, ChunkSide))};
}

int TerrainFoliageCell::IndexInChunk(const glm::ivec3& voxel)
{
    const int x = voxel.x - FloorDiv(voxel.x, ChunkSide) * ChunkSide;
    const int y = voxel.y - FloorDiv(voxel.y, ChunkSide) * ChunkSide;
    const int z = voxel.z - FloorDiv(voxel.z, ChunkSide) * ChunkSide;
    return x + ChunkSide * (y + ChunkSide * z);
}

bool TerrainFoliageCell::Voxel(const Layer& layer, const glm::ivec3& voxel, float& offset)
{
    const auto found = layer.chunks.find(ChunkOf(voxel));
    if (found == layer.chunks.end()) return false;
    offset = DecodeOffset(found->second[static_cast<size_t>(IndexInChunk(voxel))]);
    return true;
}

uint8_t TerrainFoliageCell::EncodeOffset(float offset)
{
    return static_cast<uint8_t>(std::clamp(128 + static_cast<int>(std::lround(std::clamp(offset, -1.f, 1.f) * 127.f)), 1, 255));
}

float TerrainFoliageCell::Sample(const Layer* layer, const glm::vec3& local, const Baseline& baseline)
{
    if (!layer || layer->chunks.empty()) return baseline(local);
    // Offsets trilinear between the eight voxel centres around the point.
    const glm::vec3 grid = local / VoxelSize - .5f;
    const glm::ivec3 base(glm::floor(grid));
    const glm::vec3 t = grid - glm::vec3(base);
    if (!Touches(layer, VoxelCentre(base), VoxelCentre(base + 1))) return baseline(local);
    float offset = 0.f;
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::ivec3 step(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        const float weight = (step.x ? t.x : 1.f - t.x) * (step.y ? t.y : 1.f - t.y) *
            (step.z ? t.z : 1.f - t.z);
        float value = 0.f;
        if (weight > 0.f && Voxel(*layer, base + step, value)) offset += weight * value;
    }
    return std::clamp(baseline(local) + offset, 0.f, 1.f);
}

bool TerrainFoliageCell::Touches(const Layer* layer, const glm::vec3& low, const glm::vec3& high)
{
    if (!layer || layer->chunks.empty()) return false;
    const auto a = ChunkOf(VoxelOf(low)), b = ChunkOf(VoxelOf(high));
    for (int z = std::get<2>(a); z <= std::get<2>(b); ++z)
        for (int y = std::get<1>(a); y <= std::get<1>(b); ++y)
            for (int x = std::get<0>(a); x <= std::get<0>(b); ++x)
                if (layer->chunks.count({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z)}))
                    return true;
    return false;
}

bool TerrainFoliageCell::Load(const std::string& path, TerrainFoliageCell& cell)
{
    cell.layers.clear();
    std::error_code error;
    if (path.empty() || !std::filesystem::exists(path, error)) return true;
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> packed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t at = 0;
    uint64_t magic = 0;
    uint32_t version = 0, raw = 0;
    if (!Get(packed, at, magic) || magic != FoliageMagic || !Get(packed, at, version) ||
        version != FoliageVersion || !Get(packed, at, raw))
        return false;
    std::vector<uint8_t> bytes(raw);
    mz_ulong length = raw;
    if (mz_uncompress(bytes.data(), &length, packed.data() + at, mz_ulong(packed.size() - at)) != MZ_OK ||
        length != raw)
        return false;
    at = 0;
    uint32_t layerCount = 0;
    if (!Get(bytes, at, layerCount)) return false;
    for (uint32_t i = 0; i < layerCount; ++i)
    {
        uint32_t nameLength = 0, chunkCount = 0;
        if (!Get(bytes, at, nameLength) || at + nameLength > bytes.size()) return false;
        std::string name(reinterpret_cast<const char*>(bytes.data() + at), nameLength);
        at += nameLength;
        if (!Get(bytes, at, chunkCount)) return false;
        auto& layer = cell.layers[name];
        for (uint32_t c = 0; c < chunkCount; ++c)
        {
            int16_t x = 0, y = 0, z = 0;
            if (!Get(bytes, at, x) || !Get(bytes, at, y) || !Get(bytes, at, z) ||
                at + ChunkVoxels > bytes.size())
                return false;
            auto& chunk = layer.chunks[{x, y, z}];
            std::memcpy(chunk.data(), bytes.data() + at, ChunkVoxels);
            at += ChunkVoxels;
        }
    }
    return true;
}

bool TerrainFoliageCell::Save(const std::string& path, const TerrainFoliageCell& cell)
{
    std::error_code error;
    if (cell.Empty())
    {
        std::filesystem::remove(path, error);
        return true;
    }
    std::vector<uint8_t> bytes;
    uint32_t layerCount = 0;
    for (const auto& [name, layer] : cell.layers) layerCount += layer.chunks.empty() ? 0u : 1u;
    Put(bytes, layerCount);
    for (const auto& [name, layer] : cell.layers)
    {
        if (layer.chunks.empty()) continue;
        Put(bytes, static_cast<uint32_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        Put(bytes, static_cast<uint32_t>(layer.chunks.size()));
        for (const auto& [key, chunk] : layer.chunks)
        {
            Put(bytes, std::get<0>(key)); Put(bytes, std::get<1>(key)); Put(bytes, std::get<2>(key));
            bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        }
    }
    mz_ulong length = mz_compressBound(mz_ulong(bytes.size()));
    std::vector<uint8_t> packed(length);
    if (mz_compress2(packed.data(), &length, bytes.data(), mz_ulong(bytes.size()), MZ_BEST_COMPRESSION) != MZ_OK)
        return false;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const uint64_t magic = FoliageMagic;
        const uint32_t version = FoliageVersion, raw = static_cast<uint32_t>(bytes.size());
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        file.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
        file.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(length));
        if (!file) return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

uint64_t TerrainFoliageCell::Signature() const
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    for (const auto& [name, layer] : layers)
    {
        if (layer.chunks.empty()) continue;
        mix(name.data(), name.size());
        for (const auto& [key, chunk] : layer.chunks)
        {
            mix(&key, sizeof(key));
            mix(chunk.data(), chunk.size());
        }
    }
    return hash;
}
}
