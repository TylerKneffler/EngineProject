#include "Core/Components/Terrain/TerrainVolume.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>

namespace Engine::Components
{
namespace
{
constexpr uint64_t Magic = 0x314c4f56524e5254ull; // "TRNRVOL1"

std::shared_ptr<const TerrainVolume> ReadVolume(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    size_t at = 0;
    const auto read = [&](auto& value)
    {
        if (at + sizeof(value) > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        at += sizeof(value);
        return true;
    };
    uint64_t magic = 0;
    uint32_t version = 0;
    int32_t size[3] = {};
    float spacing = 0.f, origin[3] = {}, units = 0.f;
    if (!read(magic) || !read(version) || !read(size) || !read(spacing) || !read(origin) || !read(units))
        return nullptr;
    if (magic != Magic || (version != 1 && version != 2) || spacing <= 0.f || units <= 0.f)
        return nullptr;
    for (int32_t extent : size)
        if (extent < 2 || extent > 65536) return nullptr;
    auto volume = std::make_shared<TerrainVolume>();
    volume->sizeX = size[0]; volume->sizeY = size[1]; volume->sizeZ = size[2];
    volume->spacing = spacing;
    volume->origin = {origin[0], origin[1], origin[2]};
    volume->unitsPerMetre = units;
    if (version == 2)
    {
        uint32_t chunkSamples = 0;
        if (!read(chunkSamples) || chunkSamples < 4 || chunkSamples > 128) return nullptr;
        volume->chunkSamples = static_cast<int>(chunkSamples);
    }
    const int c = volume->chunkSamples;
    volume->chunksX = (size[0] + c - 1) / c;
    volume->chunksY = (size[1] + c - 1) / c;
    volume->chunksZ = (size[2] + c - 1) / c;
    const size_t chunkCount = static_cast<size_t>(volume->chunksX) * volume->chunksY * volume->chunksZ;
    if (chunkCount > (size_t(1) << 22)) return nullptr;
    volume->chunks.resize(chunkCount);
    const size_t perChunk = static_cast<size_t>(c) * c * c;
    if (version == 2)
    {
        for (auto& chunk : volume->chunks)
        {
            uint8_t kind = 0;
            if (!read(kind)) return nullptr;
            if (kind == 0) { if (!read(chunk.uniform)) return nullptr; continue; }
            if (kind != 1 || at + perChunk * sizeof(int16_t) > bytes.size()) return nullptr;
            chunk.samples.resize(perChunk);
            std::memcpy(chunk.samples.data(), bytes.data() + at, perChunk * sizeof(int16_t));
            at += perChunk * sizeof(int16_t);
        }
        return at == bytes.size() ? volume : nullptr;
    }
    // Version 1: dense; cut into chunks (uniform where every sample agrees).
    const size_t count = static_cast<size_t>(size[0]) * size[1] * size[2];
    if (at + count * sizeof(int16_t) != bytes.size()) return nullptr;
    const auto dense = [&](int x, int y, int z)
    {
        x = std::min(x, size[0] - 1); y = std::min(y, size[1] - 1); z = std::min(z, size[2] - 1);
        int16_t value = 0;
        std::memcpy(&value, bytes.data() + at +
            ((static_cast<size_t>(y) * size[2] + z) * size[0] + x) * sizeof(int16_t), sizeof(value));
        return value;
    };
    for (int cy = 0; cy < volume->chunksY; ++cy)
        for (int cz = 0; cz < volume->chunksZ; ++cz)
            for (int cx = 0; cx < volume->chunksX; ++cx)
            {
                auto& chunk = volume->chunks[(static_cast<size_t>(cy) * volume->chunksZ + cz) * volume->chunksX + cx];
                chunk.samples.resize(perChunk);
                bool uniform = true;
                for (int y = 0; y < c; ++y)
                    for (int z = 0; z < c; ++z)
                        for (int x = 0; x < c; ++x)
                        {
                            const int16_t value = dense(cx * c + x, cy * c + y, cz * c + z);
                            chunk.samples[(static_cast<size_t>(y) * c + z) * c + x] = value;
                            uniform = uniform && value == chunk.samples[0];
                        }
                if (uniform) { chunk.uniform = chunk.samples[0]; chunk.samples.clear(); }
            }
    return volume;
}
}

float TerrainVolume::At(int x, int y, int z) const
{
    x = std::clamp(x, 0, sizeX - 1);
    y = std::clamp(y, 0, sizeY - 1);
    z = std::clamp(z, 0, sizeZ - 1);
    const int c = chunkSamples;
    const auto& chunk = chunks[(static_cast<size_t>(y / c) * chunksZ + z / c) * chunksX + x / c];
    const int16_t value = chunk.samples.empty() ? chunk.uniform
        : chunk.samples[(static_cast<size_t>(y % c) * c + z % c) * c + x % c];
    return static_cast<float>(value) / unitsPerMetre;
}

float TerrainVolume::Sample(const glm::vec3& position) const
{
    const glm::vec3 grid = (position - origin) / spacing;
    const glm::vec3 clamped = glm::clamp(grid, glm::vec3(0.f),
        glm::vec3(sizeX - 1.001f, sizeY - 1.001f, sizeZ - 1.001f));
    const glm::ivec3 cell = glm::ivec3(glm::floor(clamped));
    const glm::vec3 t = clamped - glm::vec3(cell);
    float result = 0.f;
    for (int corner = 0; corner < 8; ++corner)
    {
        const int dx = corner & 1, dy = (corner >> 1) & 1, dz = (corner >> 2) & 1;
        const float w = (dx ? t.x : 1.f - t.x) * (dy ? t.y : 1.f - t.y) * (dz ? t.z : 1.f - t.z);
        result += w * At(cell.x + dx, cell.y + dy, cell.z + dz);
    }
    return result;
}

size_t TerrainVolume::MemoryBytes() const
{
    size_t bytes = chunks.size() * sizeof(Chunk);
    for (const auto& chunk : chunks) bytes += chunk.samples.size() * sizeof(int16_t);
    return bytes;
}

std::shared_ptr<const TerrainVolume> TerrainVolume::Load(const std::string& path)
{
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<const TerrainVolume>> cache;
    std::lock_guard lock(mutex);
    if (auto found = cache[path].lock()) return found;
    auto volume = ReadVolume(path);
    cache[path] = volume;
    return volume;
}

void MeshSurfaceNets(const TerrainVolume& volume, const glm::ivec3& first,
    const glm::ivec3& cells, int stride, float skirt,
    std::vector<Mesh::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    vertices.clear();
    indices.clear();
    stride = std::max(stride, 1);
    if (cells.x < 1 || cells.y < 1 || cells.z < 1) return;
    const glm::ivec3 size(volume.sizeX, volume.sizeY, volume.sizeZ);
    // Coarse sample (i, j, k) is global sample first + (i, j, k) * stride.
    const auto global = [&](int i, int j, int k) { return first + glm::ivec3(i, j, k) * stride; };
    const auto inside = [&](const glm::ivec3& g) { return glm::all(glm::greaterThanEqual(g, glm::ivec3(0))) &&
        glm::all(glm::lessThan(g, size)); };
    const auto density = [&](int i, int j, int k)
    { const glm::ivec3 g = global(i, j, k); return volume.At(g.x, g.y, g.z); };
    // Cells -1 .. cells - 1 on every axis (the -1 layer joins the block's low
    // faces to its neighbours'); cell vertex table offset by one.
    const glm::ivec3 span = cells + 1;
    const auto slot = [&](int i, int j, int k)
    { return (static_cast<size_t>(j + 1) * span.z + (k + 1)) * span.x + (i + 1); };
    std::vector<uint32_t> cellVertex(static_cast<size_t>(span.x) * span.y * span.z, UINT32_MAX);
    constexpr std::array<std::array<int, 3>, 8> Corners{{
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}}};
    constexpr std::array<std::array<int, 2>, 12> Edges{{
        {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
    const float cellSize = volume.spacing * stride;
    for (int j = -1; j < cells.y; ++j)
        for (int k = -1; k < cells.z; ++k)
            for (int i = -1; i < cells.x; ++i)
            {
                // Both corners of the cell must lie on the grid.
                if (!inside(global(i, j, k)) || !inside(global(i + 1, j + 1, k + 1))) continue;
                std::array<float, 8> d;
                int solid = 0;
                for (int c = 0; c < 8; ++c)
                {
                    d[c] = density(i + Corners[c][0], j + Corners[c][1], k + Corners[c][2]);
                    solid += d[c] > 0.f;
                }
                if (solid == 0 || solid == 8) continue;
                glm::vec3 sum(0.f);
                int crossings = 0;
                for (const auto& edge : Edges)
                {
                    const float a = d[edge[0]], b = d[edge[1]];
                    if ((a > 0.f) == (b > 0.f)) continue;
                    const float t = a / (a - b);
                    const glm::vec3 pa(Corners[edge[0]][0], Corners[edge[0]][1], Corners[edge[0]][2]);
                    const glm::vec3 pb(Corners[edge[1]][0], Corners[edge[1]][1], Corners[edge[1]][2]);
                    sum += pa + (pb - pa) * t;
                    ++crossings;
                }
                const glm::vec3 position = volume.origin +
                    (glm::vec3(global(i, j, k)) + sum / static_cast<float>(crossings) * static_cast<float>(stride)) *
                    volume.spacing;
                // Outward normal: down the density gradient, over the level's cell.
                const float h = cellSize * .5f;
                const glm::vec3 gradient(
                    volume.Sample(position + glm::vec3(h, 0.f, 0.f)) - volume.Sample(position - glm::vec3(h, 0.f, 0.f)),
                    volume.Sample(position + glm::vec3(0.f, h, 0.f)) - volume.Sample(position - glm::vec3(0.f, h, 0.f)),
                    volume.Sample(position + glm::vec3(0.f, 0.f, h)) - volume.Sample(position - glm::vec3(0.f, 0.f, h)));
                const glm::vec3 normal = glm::dot(gradient, gradient) > 1.e-12f
                    ? -glm::normalize(gradient) : glm::vec3(0.f, 1.f, 0.f);
                Mesh::Vertex vertex{};
                vertex.pos[0] = position.x; vertex.pos[1] = position.y; vertex.pos[2] = position.z;
                vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
                vertex.uv[0] = position.x; vertex.uv[1] = position.z;
                vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.f;
                cellVertex[slot(i, j, k)] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }
    // Faces: a quad across every sign-changing edge whose base sample is in
    // the block, joining its four cells, wound so it faces the air (front
    // faces have cross(v1 - v0, v2 - v0) pointing out, as the terrain grid's).
    const auto position = [&](uint32_t index)
    { return glm::vec3(vertices[index].pos[0], vertices[index].pos[1], vertices[index].pos[2]); };
    const auto quad = [&](size_t sa, size_t sb, size_t sc, size_t sd, const glm::vec3& outward)
    {
        const uint32_t a = cellVertex[sa], b = cellVertex[sb], c = cellVertex[sc], d = cellVertex[sd];
        if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX || d == UINT32_MAX) return;
        const glm::vec3 n = glm::cross(position(b) - position(a), position(c) - position(a)) +
            glm::cross(position(c) - position(a), position(d) - position(a));
        if (glm::dot(n, outward) >= 0.f)
            indices.insert(indices.end(), {a, b, c, a, c, d});
        else
            indices.insert(indices.end(), {a, c, b, a, d, c});
    };
    for (int j = 0; j < cells.y; ++j)
        for (int k = 0; k < cells.z; ++k)
            for (int i = 0; i < cells.x; ++i)
            {
                if (!inside(global(i, j, k))) continue;
                const bool here = density(i, j, k) > 0.f;
                if (inside(global(i + 1, j, k)) && here != (density(i + 1, j, k) > 0.f))
                    quad(slot(i, j - 1, k - 1), slot(i, j, k - 1), slot(i, j, k), slot(i, j - 1, k),
                         glm::vec3(here ? 1.f : -1.f, 0.f, 0.f));
                if (inside(global(i, j + 1, k)) && here != (density(i, j + 1, k) > 0.f))
                    quad(slot(i - 1, j, k - 1), slot(i, j, k - 1), slot(i, j, k), slot(i - 1, j, k),
                         glm::vec3(0.f, here ? 1.f : -1.f, 0.f));
                if (inside(global(i, j, k + 1)) && here != (density(i, j, k + 1) > 0.f))
                    quad(slot(i - 1, j - 1, k), slot(i, j - 1, k), slot(i, j, k), slot(i - 1, j, k),
                         glm::vec3(0.f, 0.f, here ? 1.f : -1.f));
            }
    if (skirt <= 0.f || indices.empty()) return;
    // Skirts: every edge used by one triangle is open (the block's border),
    // extruded into the ground along its vertices' normals.
    std::unordered_map<uint64_t, int> uses;
    const auto key = [](uint32_t a, uint32_t b)
    { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
        for (int e = 0; e < 3; ++e)
            ++uses[key(indices[t + e], indices[t + (e + 1) % 3])];
    std::unordered_map<uint32_t, uint32_t> lowered;
    const auto lower = [&](uint32_t index)
    {
        if (const auto found = lowered.find(index); found != lowered.end()) return found->second;
        Mesh::Vertex vertex = vertices[index];
        for (int axis = 0; axis < 3; ++axis) vertex.pos[axis] -= vertex.normal[axis] * skirt;
        const uint32_t added = static_cast<uint32_t>(vertices.size());
        vertices.push_back(vertex);
        lowered.emplace(index, added);
        return added;
    };
    const size_t faces = indices.size();
    for (size_t t = 0; t + 2 < faces; t += 3)
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t a = indices[t + e], b = indices[t + (e + 1) % 3];
            if (uses[key(a, b)] != 1) continue;
            const uint32_t la = lower(a), lb = lower(b);
            // The open edge runs a -> b in its triangle; the skirt runs it
            // b -> a, so it is wound the same way round.
            indices.insert(indices.end(), {b, a, la, b, la, lb});
        }
}

void MeshSurfaceNets(const TerrainVolume& volume,
    std::vector<Mesh::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    MeshSurfaceNets(volume, glm::ivec3(0), glm::ivec3(volume.sizeX - 1, volume.sizeY - 1, volume.sizeZ - 1),
        1, 0.f, vertices, indices);
}
}
