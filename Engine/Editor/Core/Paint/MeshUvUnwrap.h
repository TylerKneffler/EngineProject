#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine::Editor::Paint
{
namespace Detail
{
inline uint64_t EdgeKey(uint32_t a, uint32_t b)
{
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}
}

// Cut UV islands at marked edges and at edges whose adjacent face normals
// exceed the seam angle. Island vertices are duplicated so the mesh can carry
// discontinuous UVs without changing positions, colors, or skin weights.
template<class Vertex>
bool UnwrapMeshUv(std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices, int uvSet, float seamAngleDegrees,
    const std::vector<std::pair<uint32_t, uint32_t>>& markedSeams = {})
{
    if ((uvSet != 0 && uvSet != 1) || vertices.empty()) return false;
    std::vector<uint32_t> source = indices;
    if (source.empty())
    {
        source.resize(vertices.size());
        std::iota(source.begin(), source.end(), 0u);
    }
    if (source.size() < 3 || source.size() % 3 != 0) return false;
    for (uint32_t id : source)
        if (id >= vertices.size()) return false;
    const size_t faceCount = source.size() / 3;
    std::vector<std::array<float, 3>> normals(faceCount);
    for (size_t face = 0; face < faceCount; ++face)
    {
        const auto& a = vertices[source[face * 3]];
        const auto& b = vertices[source[face * 3 + 1]];
        const auto& c = vertices[source[face * 3 + 2]];
        const float ab[3]{ b.pos[0] - a.pos[0], b.pos[1] - a.pos[1],
            b.pos[2] - a.pos[2] };
        const float ac[3]{ c.pos[0] - a.pos[0], c.pos[1] - a.pos[1],
            c.pos[2] - a.pos[2] };
        auto& n = normals[face];
        n = { ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0] };
        const float length = std::hypot(n[0], n[1], n[2]);
        if (length <= 1e-8f) return false;
        for (float& value : n) value /= length;
    }
    std::unordered_set<uint64_t> seams;
    for (const auto& [a, b] : markedSeams)
        seams.insert(Detail::EdgeKey(a, b));
    std::vector<size_t> parent(faceCount);
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](size_t face)
    {
        while (parent[face] != face)
        { parent[face] = parent[parent[face]]; face = parent[face]; }
        return face;
    };
    std::unordered_map<uint64_t, std::vector<size_t>> edgeFaces;
    for (size_t face = 0; face < faceCount; ++face)
        for (int corner = 0; corner < 3; ++corner)
            edgeFaces[Detail::EdgeKey(source[face * 3 + corner],
                source[face * 3 + (corner + 1) % 3])].push_back(face);
    const float angle = std::clamp(seamAngleDegrees, 0.f, 180.f) *
        .017453292519943295f;
    const float minDot = std::cos(angle);
    for (const auto& [edge, faces] : edgeFaces)
    {
        if (faces.size() != 2 || seams.find(edge) != seams.end()) continue;
        const auto& a = normals[faces[0]];
        const auto& b = normals[faces[1]];
        const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        if (dot < minDot) continue;
        parent[root(faces[1])] = root(faces[0]);
    }
    struct Island
    {
        std::vector<size_t> faces;
        int axis = 2;
        float minU = 0.f, minV = 0.f, width = 0.f, height = 0.f;
        float offsetU = 0.f, offsetV = 0.f;
    };
    std::vector<Island> islands;
    std::unordered_map<size_t, size_t> islandForRoot;
    std::vector<size_t> faceIsland(faceCount);
    for (size_t face = 0; face < faceCount; ++face)
    {
        const size_t r = root(face);
        auto [entry, inserted] = islandForRoot.try_emplace(r, islands.size());
        if (inserted) islands.emplace_back();
        faceIsland[face] = entry->second;
        islands[entry->second].faces.push_back(face);
    }
    const auto project = [](const Vertex& vertex, int axis)
    {
        if (axis == 0) return std::pair{ vertex.pos[2], vertex.pos[1] };
        if (axis == 1) return std::pair{ vertex.pos[0], vertex.pos[2] };
        return std::pair{ vertex.pos[0], vertex.pos[1] };
    };
    float largest = 0.f;
    for (Island& island : islands)
    {
        std::array<float, 3> average{};
        for (size_t face : island.faces)
            for (int axis = 0; axis < 3; ++axis)
                average[axis] += normals[face][axis];
        island.axis = static_cast<int>(std::max_element(average.begin(),
            average.end(), [](float a, float b)
            { return std::abs(a) < std::abs(b); }) - average.begin());
        float maxU = -INFINITY, maxV = -INFINITY;
        island.minU = island.minV = INFINITY;
        for (size_t face : island.faces)
            for (int corner = 0; corner < 3; ++corner)
            {
                const auto [u, v] = project(vertices[
                    source[face * 3 + corner]], island.axis);
                island.minU = std::min(island.minU, u);
                island.minV = std::min(island.minV, v);
                maxU = std::max(maxU, u);
                maxV = std::max(maxV, v);
            }
        island.width = std::max(maxU - island.minU, 1e-5f);
        island.height = std::max(maxV - island.minV, 1e-5f);
        largest = std::max({ largest, island.width, island.height });
    }
    std::vector<size_t> order(islands.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
    { return islands[a].height > islands[b].height; });
    constexpr float padding = .01f;
    const auto pack = [&](float scale)
    {
        float x = 0.f, y = 0.f, rowHeight = 0.f;
        for (size_t id : order)
        {
            Island& island = islands[id];
            const float boxWidth = island.width * scale + padding * 2.f;
            const float boxHeight = island.height * scale + padding * 2.f;
            if (x + boxWidth > 1.f + 1e-6f)
            { x = 0.f; y += rowHeight; rowHeight = 0.f; }
            if (boxWidth > 1.f || y + boxHeight > 1.f + 1e-6f)
                return false;
            island.offsetU = x + padding;
            island.offsetV = y + padding;
            x += boxWidth;
            rowHeight = std::max(rowHeight, boxHeight);
        }
        return true;
    };
    float low = 0.f, high = 1.f / largest;
    for (int step = 0; step < 32; ++step)
    {
        const float mid = (low + high) * .5f;
        if (pack(mid)) low = mid;
        else high = mid;
    }
    if (!pack(low) || low <= 0.f) return false;

    std::vector<Vertex> output;
    output.reserve(source.size());
    std::vector<uint32_t> outputIndices;
    outputIndices.reserve(source.size());
    std::unordered_map<uint64_t, uint32_t> mapped;
    for (size_t face = 0; face < faceCount; ++face)
    {
        const size_t islandId = faceIsland[face];
        const Island& island = islands[islandId];
        for (int corner = 0; corner < 3; ++corner)
        {
            const uint32_t id = source[face * 3 + corner];
            const uint64_t key = (static_cast<uint64_t>(islandId) << 32) | id;
            auto found = mapped.find(key);
            if (found == mapped.end())
            {
                Vertex vertex = vertices[id];
                const auto [u, v] = project(vertex, island.axis);
                float* uv = uvSet == 0 ? vertex.uv : vertex.uv1;
                uv[0] = island.offsetU + (u - island.minU) * low;
                uv[1] = island.offsetV + (v - island.minV) * low;
                const uint32_t next = static_cast<uint32_t>(output.size());
                output.push_back(vertex);
                found = mapped.emplace(key, next).first;
            }
            outputIndices.push_back(found->second);
        }
    }
    vertices = std::move(output);
    indices = std::move(outputIndices);
    return true;
}
}
