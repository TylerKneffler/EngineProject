#include "TerrainManager.h"
#include "TerrainManagerFarTerrainCache.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include <cstring>
#include <fstream>

namespace Engine::Components::FarTerrainInternal
{
constexpr uint64_t FarTileCacheMagic = 0x31454c4954524146ull;
constexpr uint32_t FarTileCacheVersion = 4;
void HashBytes(uint64_t& hash, const void* data, size_t bytes)
{
    const auto* begin = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ begin[i]) * 1099511628211ull;
}
struct CachedFarVertex
{
    float pos[3], normal[3], uv[2], color[4];
};
static_assert(sizeof(CachedFarVertex) == sizeof(float) * 12);
struct FarTileCacheHeader
{
    uint64_t magic = FarTileCacheMagic;
    uint32_t version = FarTileCacheVersion;
    uint32_t vertexCount = 0, indexCount = 0;
    uint64_t fingerprint = 0;
    uint64_t payloadHash = 0;
};
bool ReadFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, std::vector<Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto bytes = file.tellg();
    file.seekg(0);
    FarTileCacheHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != FarTileCacheMagic ||
        header.version != FarTileCacheVersion ||
        header.fingerprint != fingerprint ||
        header.vertexCount == 0 || header.vertexCount > 200000 ||
        header.indexCount == 0 || header.indexCount > 1200000 ||
        bytes != static_cast<std::streamoff>(sizeof(header) +
            static_cast<size_t>(header.vertexCount) * sizeof(CachedFarVertex) +
            static_cast<size_t>(header.indexCount) * sizeof(uint32_t)))
        return false;
    std::vector<CachedFarVertex> compact(header.vertexCount);
    indices.resize(header.indexCount);
    file.read(reinterpret_cast<char*>(compact.data()),
        compact.size() * sizeof(CachedFarVertex));
    file.read(reinterpret_cast<char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    if (!file) return false;
    uint64_t payloadHash = 14695981039346656037ull;
    HashBytes(payloadHash, compact.data(),
        compact.size() * sizeof(CachedFarVertex));
    HashBytes(payloadHash, indices.data(), indices.size() * sizeof(uint32_t));
    if (payloadHash != header.payloadHash) return false;
    for (uint32_t index : indices)
        if (index >= compact.size()) return false;
    vertices.resize(compact.size());
    for (size_t i = 0; i < compact.size(); ++i)
    {
        std::memcpy(vertices[i].pos, compact[i].pos, sizeof(compact[i].pos));
        std::memcpy(vertices[i].normal, compact[i].normal,
            sizeof(compact[i].normal));
        std::memcpy(vertices[i].uv, compact[i].uv, sizeof(compact[i].uv));
        std::memcpy(vertices[i].color, compact[i].color,
            sizeof(compact[i].color));
    }
    return true;
}
void WriteFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, const std::vector<Mesh::Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    std::vector<CachedFarVertex> compact;
    compact.reserve(vertices.size());
    for (const auto& vertex : vertices)
    {
        compact.push_back({{vertex.pos[0], vertex.pos[1], vertex.pos[2]},
            {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
            {vertex.uv[0], vertex.uv[1]},
            {vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]}});
    }
    uint64_t payloadHash = 14695981039346656037ull;
    HashBytes(payloadHash, compact.data(),
        compact.size() * sizeof(CachedFarVertex));
    HashBytes(payloadHash, indices.data(), indices.size() * sizeof(uint32_t));
    const FarTileCacheHeader header{FarTileCacheMagic, FarTileCacheVersion,
        static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()), fingerprint, payloadHash};
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(compact.data()),
        compact.size() * sizeof(CachedFarVertex));
    file.write(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    file.close();
    if (!file) return;
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}
}

