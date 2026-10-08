#include "MeshEditGeometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>
#include <glm/glm.hpp>

namespace Engine::Editor
{
namespace
{
using Edge = std::pair<uint32_t, uint32_t>;
Edge SortedEdge(uint32_t a, uint32_t b)
{ return std::minmax(a, b); }

std::vector<Edge> Edges(const std::vector<uint32_t>& indices)
{
    std::set<Edge> unique;
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        unique.insert(SortedEdge(indices[i], indices[i + 1]));
        unique.insert(SortedEdge(indices[i + 1], indices[i + 2]));
        unique.insert(SortedEdge(indices[i + 2], indices[i]));
    }
    return { unique.begin(), unique.end() };
}

glm::vec3 Position(const MeshEditGeometry::Vertex& v)
{ return { v.pos[0], v.pos[1], v.pos[2] }; }

void SetPosition(MeshEditGeometry::Vertex& v, glm::vec3 p)
{ v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z; }

MeshEditGeometry::Vertex Blend(const MeshEditGeometry::Vertex& a,
    const MeshEditGeometry::Vertex& b, float fraction)
{
    MeshEditGeometry::Vertex result = a;
    const auto blend = [fraction](float* destination, const float* other,
        size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            destination[i] += (other[i] - destination[i]) * fraction;
    };
    blend(result.pos, b.pos, 3);
    blend(result.normal, b.normal, 3);
    blend(result.uv, b.uv, 2);
    blend(result.tangent, b.tangent, 4);
    blend(result.uv1, b.uv1, 2);
    blend(result.color, b.color, 4);
    // A midpoint can have a different joint palette on either endpoint.
    // Preserve a valid influence set until weight painting handles remapping.
    return result;
}
}

MeshEditGeometry MeshEditGeometry::FromMesh(const Engine::Components::Mesh& mesh)
{
    MeshEditGeometry result{ mesh.GetVertices(), mesh.GetIndices() };
    if (result.indices.empty())
    {
        result.indices.reserve(result.vertices.size());
        for (uint32_t i = 0; i < result.vertices.size(); ++i)
            result.indices.push_back(i);
    }
    return result;
}

uint32_t MeshEditGeometry::EdgeCount() const
{ return static_cast<uint32_t>(Edges(indices).size()); }

bool MeshEditGeometry::ExtrudeFace(uint32_t face, float distance)
{
    if (face >= FaceCount() || !std::isfinite(distance) || distance == 0.f)
        return false;
    const size_t offset = static_cast<size_t>(face) * 3;
    const std::array<uint32_t, 3> base{
        indices[offset], indices[offset + 1], indices[offset + 2] };
    const glm::vec3 a = Position(vertices[base[0]]);
    const glm::vec3 b = Position(vertices[base[1]]);
    const glm::vec3 c = Position(vertices[base[2]]);
    const glm::vec3 cross = glm::cross(b - a, c - a);
    if (glm::dot(cross, cross) < 1e-12f) return false;
    const glm::vec3 translation = glm::normalize(cross) * distance;
    std::array<uint32_t, 3> top{};
    for (size_t i = 0; i < 3; ++i)
    {
        top[i] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(vertices[base[i]]);
        SetPosition(vertices.back(), Position(vertices.back()) + translation);
    }
    indices[offset] = top[0];
    indices[offset + 1] = top[1];
    indices[offset + 2] = top[2];
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t next = (i + 1) % 3;
        indices.insert(indices.end(), { base[i], base[next], top[next],
            base[i], top[next], top[i] });
    }
    return true;
}

bool MeshEditGeometry::InsetFace(uint32_t face, float amount)
{
    if (face >= FaceCount() || !std::isfinite(amount) ||
        amount <= 0.f || amount >= 1.f) return false;
    const size_t offset = static_cast<size_t>(face) * 3;
    const std::array<uint32_t, 3> base{
        indices[offset], indices[offset + 1], indices[offset + 2] };
    std::array<uint32_t, 3> inner{};
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& a = vertices[base[i]];
        const auto& b = vertices[base[(i + 1) % 3]];
        const auto& c = vertices[base[(i + 2) % 3]];
        Vertex mid = Blend(Blend(a, b, .5f), c, 1.f / 3.f);
        inner[i] = static_cast<uint32_t>(vertices.size());
        vertices.push_back(Blend(a, mid, amount));
    }
    indices[offset] = inner[0];
    indices[offset + 1] = inner[1];
    indices[offset + 2] = inner[2];
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t next = (i + 1) % 3;
        indices.insert(indices.end(), { base[i], base[next], inner[next],
            base[i], inner[next], inner[i] });
    }
    return true;
}

bool MeshEditGeometry::SplitEdge(uint32_t edge)
{
    const auto edges = Edges(indices);
    if (edge >= edges.size()) return false;
    const auto [a, b] = edges[edge];
    const uint32_t midpoint = static_cast<uint32_t>(vertices.size());
    vertices.push_back(Blend(vertices[a], vertices[b], .5f));
    std::vector<uint32_t> output;
    output.reserve(indices.size() + 6);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const std::array<uint32_t, 3> triangle{
            indices[i], indices[i + 1], indices[i + 2] };
        bool split = false;
        for (size_t j = 0; j < 3; ++j)
        {
            const uint32_t first = triangle[j];
            const uint32_t second = triangle[(j + 1) % 3];
            if (SortedEdge(first, second) != edges[edge]) continue;
            const uint32_t third = triangle[(j + 2) % 3];
            output.insert(output.end(), { first, midpoint, third,
                midpoint, second, third });
            split = true;
            break;
        }
        if (!split) output.insert(output.end(), triangle.begin(), triangle.end());
    }
    indices = std::move(output);
    return true;
}

bool MeshEditGeometry::DeleteFace(uint32_t face)
{
    if (face >= FaceCount() || FaceCount() <= 1) return false;
    indices.erase(indices.begin() + static_cast<size_t>(face) * 3,
        indices.begin() + static_cast<size_t>(face) * 3 + 3);
    return true;
}
}
