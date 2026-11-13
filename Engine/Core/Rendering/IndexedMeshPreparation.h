#pragma once
#include "SharedMeshBuffers.h"
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace Engine::Rendering
{
// Load-time conversion only. Exact full-vertex equality preserves UV seams,
// normals, tangents, colors and auxiliary attributes as well as triangle order.
// Leave unshared triangle lists alone when indices would increase storage.
template<class Vertex>
bool IndexTriangleList(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    static_assert(std::is_trivially_copyable_v<Vertex>);
    if (!indices.empty() || vertices.empty() || vertices.size() % 3 ||
        vertices.size() > std::numeric_limits<uint32_t>::max()) return false;
    std::unordered_multimap<uint64_t, uint32_t> buckets;
    buckets.reserve(vertices.size());
    std::vector<Vertex> unique;
    unique.reserve(vertices.size());
    std::vector<uint32_t> remap;
    remap.reserve(vertices.size());
    for (const auto& vertex : vertices)
    {
        const auto hash = HashMeshBuffer(reinterpret_cast<const unsigned char*>(&vertex), sizeof(Vertex));
        const auto [begin, end] = buckets.equal_range(hash);
        uint32_t index = static_cast<uint32_t>(unique.size());
        for (auto it = begin; it != end; ++it)
            if (std::memcmp(&unique[it->second], &vertex, sizeof(Vertex)) == 0)
            { index = it->second; break; }
        if (index == unique.size())
        {
            unique.push_back(vertex);
            buckets.emplace(hash, index);
        }
        remap.push_back(index);
    }
    if (unique.size() * sizeof(Vertex) + remap.size() * sizeof(uint32_t)
        >= vertices.size() * sizeof(Vertex)) return false;
    // Release the original-sized reservation before retaining this prototype.
    unique.shrink_to_fit();
    vertices = std::move(unique);
    indices = std::move(remap);
    return true;
}
}
