#pragma once

#include "Core/Compoonents/Mesh/Mesh.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine::Components::FarTerrainInternal
{
void HashBytes(uint64_t& hash, const void* data, size_t bytes);
template<class T> void HashValue(uint64_t& hash, const T& value)
{ HashBytes(hash, &value, sizeof(value)); }
bool ReadFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, std::vector<Mesh::Vertex>& vertices,
    std::vector<uint32_t>& indices);
void WriteFarTileCache(const std::filesystem::path& path,
    uint64_t fingerprint, const std::vector<Mesh::Vertex>& vertices,
    const std::vector<uint32_t>& indices);
}
